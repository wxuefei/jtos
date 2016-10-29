#include <stdint.h>

#include "paging.h"
#include "address.h"
#include "common.h"
#include "console.h"
#include "efi.h"
#include "framebuffer.h"
#include "pagealloc.h"
#include "serial.h"

/**
 * Page Table size
 *
 * = PD size = PDPT size = PML4 size
 */
#define PAGE_TABLE_SIZE 512

/**
 * PE flags; Page Entry flags
 *
 * Page Table Entry; PTE (-> 4k) /
 * Page Directory Entry; PDE (-> PT) /
 * Page Directory Pointer Table Entry; PDPTE (-> PD) /
 * Page Mapping Level 4 Entry; PML4E (-> PDPT)
 */
typedef struct {
	/**
	 * P; Present
	 */
	uint8_t p : 1;
	/**
	 * R/W; Read/write
	 */
	uint8_t rw : 1;
	/**
	 * U/S; User/supervisor
	 */
	uint8_t us : 1;
	/**
	 * PWT; Page-level write-through
	 */
	uint8_t pwt : 1;
	/**
	 * PCD; Page-level cache disable
	 */
	uint8_t pcd : 1;
	/**
	 * A; Accessed
	 */
	uint8_t a : 1;
	/**
	 * PTE: D; Dirty
	 * PDE/PDPTE/PML4E: Ignored
	 */
	uint8_t d : 1;
	/**
	 * PTE: PAT
	 * PDE/PDPTE/PML4E: PS; Page size
	 */
	uint8_t pat_ps : 1;
	/**
	 * PTE: G; Global
	 * PDE/PDPTE/PML4E: Ignored
	 */
	uint8_t g : 1;
	/**
	 * Ignored
	 */
	uint8_t ignored_0 : 3;
	/**
	 * Physical address
	 */
	uint64_t phy : 40;
	/**
	 * Ignored
	 */
	uint8_t ignored_1 : 7;
	/**
	 * PTE: Protection key
	 * PDE/PDPTE/PML4E: Ignored
	 */
	uint8_t pk : 4;
	/**
	 * XD; Execution-disable
	 */
	uint8_t xd : 1;
} PACKED Pe;

_Static_assert(sizeof(Pe) == sizeof(uint64_t), "wrong size of Pe structure");

Pe pml4[PAGE_TABLE_SIZE] PAGE_ALIGNED;

static inline uint64_t read_cr3()
{
	uint64_t value;
	__asm__("movq %%cr3, %0" : "=r"(value));
	return value;
}

static inline void write_cr3(uint64_t value)
{
	__asm__("movq %0, %%cr3" :: "r"(value));
}

static LinAddr linaddr(uint64_t a)
{
	LinAddr *linaddrp = (LinAddr *) &a;
	return *linaddrp;
}

static void init_pe(Pe *pe, uint64_t ppa)
{
	pe->phy = ppa >> PAGE_SIZE_BITS;
	pe->p = 1;
	pe->rw = 1;
}

// get page map
static void * get_pm(Pe *pt, uint64_t pi) {
	serial_print("> get_pm\r\n");
	Pe *pe = &pt[pi];
	if(pe->p == 0) {
		serial_print("pgalloc (\r\n");
		void *page = pgalloc();
		serial_print("pgalloc )\r\n");
		init_pe(pe, (uint64_t) page);

		serial_print("< get_pm\r\n");
		return page;
	} else {
		void *page = (void *)(pe->phy << PAGE_SIZE_BITS);

		serial_print("< get_pm\r\n");
		return page;
	}
}

static void map_page_pt(Pe pt[], uint64_t vpa, uint64_t ppa)
{
	Pe *pe = &pt[linaddr(vpa).pt];
	// kassert(pe->phy == 0);
	// kassert(pe->phy == 1);
	init_pe(pe, ppa);
}

static void map_page_pd(Pe pd[], uint64_t vpa, uint64_t ppa)
{
	void *pt = get_pm(pd, linaddr(vpa).pd);
	map_page_pt(pt, vpa, ppa);
}

static void map_page_pdpt(Pe pdpt[], uint64_t vpa, uint64_t ppa)
{
	void *pd = get_pm(pdpt, linaddr(vpa).pdpt);
	map_page_pd(pd, vpa, ppa);
}

static void map_page_pml4(Pe pml4[], uint64_t vpa, uint64_t ppa)
{
	serial_print("> map_page_pml4\r\n");
	void *pdpt = get_pm(pml4, linaddr(vpa).pml4);
	map_page_pdpt(pdpt, vpa, ppa);
	serial_print("< map_page_pml4\r\n");
}

static void map_region(uint64_t vra, uint64_t pra, int np)
{
	serial_print("> map_region\r\n");
	// kassert(vra & OFFSET_MASK == 0)
	// kassert(pra & OFFSET_MASK == 0)

	uint64_t vrae = vra + np * PAGE_SIZE; // virtual address end
	for(; vra < vrae; vra += PAGE_SIZE, pra += PAGE_SIZE) {
		map_page_pml4(pml4, vra, pra);
	}

	serial_print("< map_region\r\n");
}

static void map_region_id(uint64_t a, int np)
{
	serial_print("> map_region_id\r\n");
	map_region(a, a, np);
	serial_print("< map_region_id\r\n");
}

static void enable_paging()
{
	write_cr3((uint64_t) pml4);
}

static void map_efi_regions(EfiMemoryMap *mm)
{
	serial_print("> map_efi_regions\r\n");
	const EFI_MEMORY_DESCRIPTOR *md = mm->memory_map;
	const UINT64 ds = mm->descriptor_size;
	for(int i = 0; i < (int) mm->memory_map_size; ++i, md = next_md(md, ds)) {
		if(md->Attribute & EFI_MEMORY_RUNTIME) {
			map_region_id(md->PhysicalStart, md->NumberOfPages); // TODO: no id
		} else if(md->Type == EfiLoaderCode) {
			map_region_id(md->PhysicalStart, md->NumberOfPages);
		} else if(md->Type == EfiLoaderData) {
			map_region_id(md->PhysicalStart, md->NumberOfPages);
		}
	}
	serial_print("< map_efi_regions\r\n");
}

static void map_framebuffer(Framebuffer *fb)
{
	serial_print("> map_framebuffer\r\n");
	uint64_t fbb = (uint64_t) fb->base;
	map_region_id(fbb, fb->size / PAGE_SIZE + 1);
	serial_print("< map_framebuffer\r\n");
}

static void map_kernel(
	uint64_t kernel_va, uint64_t kernel_pa, uint64_t kernel_size)
{
	serial_print(">>>>>>>>>>>>>>>>>>>>>>>> map_kernel\r\n");
	map_region(kernel_va, kernel_pa, kernel_size >> PAGE_SIZE_BITS);
	serial_print("<<<<<<<<<<<<<<<<<<<<<<<< map_kernel\r\n");
}

void *memset(void *dest, int e, unsigned long len) {
    uint8_t *d = dest;
    for(uint64_t i = 0; i < len; i++, d++) {
        *d = e;
    }
    return dest;
}

void setup_paging(
	EfiMemoryMap *mm, Framebuffer *fb, uint64_t kernel_pa, uint64_t kernel_va)
{
	memset(pml4, 0, sizeof(pml4));
	
	const size_t kernel_est_size = 1024 * 1024; // HACK: img size + ~.bss
	// map_kernel(kernel_va, kernel_pa, kernel_est_size);
	map_efi_regions(mm);
	// map_framebuffer(fb);
	serial_print("enable_paging (\r\n");
	enable_paging();
	serial_print("enable_paging )\r\n");
}
