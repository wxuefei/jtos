#include "kernel.h"
#include "common.h"
#include "console.h"
#include "efi.h"
#include "paging.h"
#include "serial.h"

static void print_status(EFI_STATUS status)
{
    #define CASE(x) case x: console_print("status = " #x "\n"); break;
    switch(status) {
    CASE(EFI_SUCCESS)
    CASE(EFI_BUFFER_TOO_SMALL)
    CASE(EFI_OUT_OF_RESOURCES)
    CASE(EFI_INVALID_PARAMETER)
    CASE(EFI_DEVICE_ERROR)
    default: console_print("status = <unknown>\n");
    }
    #undef CASE
}

static void print_time(EFI_RUNTIME_SERVICES *rts)
{
	EFI_TIME time;
	EFI_STATUS status = rts->GetTime(&time, NULL);
	if(status == EFI_SUCCESS) {
		console_print("* time: 0x");
		console_print_u8(time.Hour);
		console_print(":0x");
		console_print_u8(time.Minute);
		console_print("\n");
		console_print("* TimeZone = ");
		console_print_u16(time.TimeZone);
		console_print("\n");
	} else {
		print_status(status);
	}
}

void kernel_main(KernelParams *_params)
{
	serial_print("> kernel_main\r\n");
	KernelParams params = *_params;

	init_serial();
	console_init(&params.fb);
	console_print("### jtos 0.0.1 alpha ###\n");

	// enable_paging(&params.efi_mm, &params.fb);
	console_print("* enabled paging\n");

	print_time(params.efi_rts);

	for(;;);
}
