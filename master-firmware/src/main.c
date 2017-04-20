#include <stdio.h>
#include <string.h>
#include <math.h>

#include <hal.h>
#include <chprintf.h>
#include <lwip/netif.h>
#include <lwip/dhcp.h>

#include "main.h"
#include "commands.h"
#include "sntp/sntp.h"
#include "unix_timestamp.h"
#include "panic_log.h"
#include "log.h"
#include <arm-cortex-tools/mpu.h>
#include <arm-cortex-tools/fault.h>
#include "blocking_uart.h"
#include "rpc_server.h"
#include "uavcan_node.h"
#include <timestamp/timestamp_stm32.h>
#include "config.h"
#include <parameter/parameter_msgpack.h>
#include <cmp_mem_access/cmp_mem_access.h>
#include "motor_manager.h"
#include "stream.h"
#include "malloc_lock.h"
#include "lwipthread.h"
#include <error/error.h>
#include "usbconf.h"
#include "base/encoder.h"
#include "base/base_controller.h"
#include "arms/arms_controller.h"
#include "arms/hands_controller.h"
#include "trace/trace_points.h"
#include "strategy.h"
#include "filesystem.h"
#include "http/server.h"
#include "can/hand_driver.h"


void init_base_motors(void);
void init_arm_motors(void);
void init_hands(void);

/* Command line related */
THD_WORKING_AREA(shell_wa, 2048);

static const ShellConfig shell_cfg1 = {
    (BaseSequentialStream *)&SD3,
    commands
};

motor_manager_t motor_manager;

// debug UART
#define DEBUG_UART_BAUDRATE 921600
static const SerialConfig debug_uart_config = {
    .speed = DEBUG_UART_BAUDRATE,
    .cr1 = 0,
    .cr2 = USART_CR2_STOP1_BITS | USART_CR2_LINEN,
    .cr3 = 0
};

void fault_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panic_log_vprintf(fmt, ap);
    va_end(ap);
}

/* Bus related declarations */
messagebus_t bus;
MUTEX_DECL(bus_lock);
CONDVAR_DECL(bus_condvar);

/**
 * Function called on a kernel panic.
 * @param [in] reaon Kernel panic message.  */
void panic_hook(const char *reason)
{
    trace(TRACE_POINT_PANIC);
    palSetPad(GPIOF, GPIOF_LED_READY);
    palSetPad(GPIOF, GPIOF_LED_DEBUG);
    palSetPad(GPIOF, GPIOF_LED_ERROR);
    palSetPad(GPIOF, GPIOF_LED_POWER_ERROR);
    palSetPad(GPIOF, GPIOF_LED_PC_ERROR);
    palSetPad(GPIOF, GPIOF_LED_BUS_ERROR);
    palSetPad(GPIOF, GPIOF_LED_YELLOW_1);
    palSetPad(GPIOF, GPIOF_LED_YELLOW_2);
    palSetPad(GPIOF, GPIOF_LED_GREEN_1);
    palSetPad(GPIOF, GPIOF_LED_GREEN_2);
    palClearPad(GPIOC, GPIOC_LED);

    panic_log_write(reason);
    if (ch.rlist.r_current != NULL) {
        panic_log_printf("\ncurrent thread: ");
        if (ch.rlist.r_current->p_name != NULL) {
            panic_log_printf("%s\n", ch.rlist.r_current->p_name);
        } else {
            panic_log_printf("0x%p\n", ch.rlist.r_current);
        }
    }
    BlockingUARTDriver panic_uart;
    blocking_uart_init(&panic_uart, USART3, DEBUG_UART_BAUDRATE);

    // block to preserve fault state
    const char *msg = panic_log_read();
    while(1) {
        if (msg != NULL) {
            chprintf((BaseSequentialStream *)&panic_uart, "kernel panic:\n%s\n", msg);
        }
        unsigned int i = 100000000;
        while(i--) {
            __asm__ volatile ("nop");
        }
    }
}

/** Late init hook, called before c++ static constructors. */
void __late_init(void)
{
    /* Enable fault handlers. */
    fault_init();

    /* Initalize memory protection unit and add a guard against NULL
     * dereferences. */
    mpu_init();

    /* Initialize and enable trace system */
    trace_init();
    trace_enable();

    /* C++ Static initializer requires working chibios. */
    halInit();
    chSysInit();
    malloc_lock_init();
}

void config_load_err_cb(void *arg, const char *id, const char *err)
{
    (void)arg;
    WARNING("parameter %s: %s", id == NULL ? "(...)" : id, err);
}

extern unsigned char config_msgpack[];
extern const size_t config_msgpack_size;

void config_load_from_flash(void)
{
    cmp_ctx_t cmp;
    cmp_mem_access_t mem;
    cmp_mem_access_ro_init(&cmp, &mem, config_msgpack, config_msgpack_size);
    parameter_msgpack_read_cmp(&global_config, &cmp, config_load_err_cb, NULL);
}

static void blink_thd(void *p)
{
    (void) p;

    while (true) {
        palTogglePad(GPIOF, GPIOF_LED_PC_ERROR);
        chThdSleepMilliseconds(200);
    }
}

static void blink_start(void)
{
    static THD_WORKING_AREA(wa, 512);
    chThdCreateStatic(wa, sizeof(wa), LOWPRIO, blink_thd, NULL);
}

/** Application entry point.  */
int main(void) {
    static thread_t *shelltp = NULL;

    /* Initializes a serial driver.  */
    sdStart(&SD3, &debug_uart_config);

    blink_start();

    /* Try to mount the filesystem. */
    filesystem_start();

    log_init();

    NOTICE("boot");

    /* Initializes a serial-over-USB CDC driver.  */
    sduObjectInit(&SDU1);
    sduStart(&SDU1, &serusbcfg);

    /*
     * Activates the USB driver and then the USB bus pull-up on D+.
     * Note, a delay is inserted in order to not have to disconnect the cable
     * after a reset.
     */
    usbDisconnectBus(serusbcfg.usbp);
    chThdSleepMilliseconds(1500);
    usbStart(serusbcfg.usbp, &usbcfg);
    usbConnectBus(serusbcfg.usbp);

    /* Shell manager initialization.  */
    shellInit();

    /* Initialize the interthread communication bus. */
    messagebus_init(&bus, &bus_lock, &bus_condvar);

    /* Initialize global objects. */
    config_init();

    /* Initialise timestamp module */
    timestamp_stm32_init();


    /* bus enumerator init */
    static __attribute__((section(".ccm"))) struct bus_enumerator_entry_allocator
                    bus_enum_entries_alloc[MAX_NB_BUS_ENUMERATOR_ENTRIES];

    bus_enumerator_init(&bus_enumerator,
                        bus_enum_entries_alloc,
                        MAX_NB_BUS_ENUMERATOR_ENTRIES);


    /* allocate and init motor manager */
    static __attribute__((section(".ccm"))) trajectory_t trajectory_buffer[MAX_NB_TRAJECTORY_BUFFERS];
    static __attribute__((section(".ccm"))) float trajectory_points_buffer[ACTUATOR_TRAJECTORY_NB_POINTS
                                                                           * ACTUATOR_TRAJECTORY_POINT_DIMENSION
                                                                           * MAX_NB_TRAJECTORY_BUFFERS];

    static __attribute__((section(".ccm"))) motor_driver_t motor_driver_buffer[MAX_NB_MOTOR_DRIVERS];

    motor_manager_init(&motor_manager,
                       trajectory_buffer,
                       MAX_NB_TRAJECTORY_BUFFERS,
                       trajectory_points_buffer,
                       MAX_NB_TRAJECTORY_BUFFERS,
                       motor_driver_buffer,
                       MAX_NB_MOTOR_DRIVERS,
                       &bus_enumerator);


    struct netif *ethernet_if;

    ip_thread_init();

    chThdSleepMilliseconds(1000);
    ethernet_if = netif_find("ms0");
    if (ethernet_if) {
        dhcp_start(ethernet_if);
    }

    sntp_init();
    rpc_server_init();
    message_server_init();
    http_server_start();

    init_base_motors();
#ifdef DEBRA
    chThdSleepMilliseconds(100);
    init_arm_motors();
    init_hands();
#endif
    config_load_from_flash();
    uavcan_node_start(10);

    /* Base init */
    encoder_start();
    robot_init();
    base_controller_start();
    position_manager_start();
    trajectory_manager_start();

    /* Arms init */
#ifdef DEBRA
    chThdSleepMilliseconds(5000);

    arms_init();
    arms_controller_start();

    hand_driver_init();
    hands_init();
    hands_controller_start();
#endif

    /* Initialize strategy thread, will wait for signal to begin game */
    strategy_start();

    stream_init();

    /* main thread, spawns a shell on USB connection. */
    while (1) {
        if (!shelltp) {
            shelltp = shellCreateStatic(&shell_cfg1, &shell_wa, sizeof(shell_wa), USB_SHELL_PRIO);
        } else if (chThdTerminatedX(shelltp)) {
            chThdRelease(shelltp);    /* Recovers memory of the previous shell.   */
            shelltp = NULL;           /* Triggers spawning of a new shell.        */
        }

        chThdSleepMilliseconds(500);
    }
}

uintptr_t __stack_chk_guard = 0xdeadbeef;

void init_base_motors(void)
{
    motor_manager_create_driver(&motor_manager, "left-wheel");
    motor_manager_create_driver(&motor_manager, "right-wheel");
}

void init_arm_motors(void)
{
    motor_manager_create_driver(&motor_manager, "left-z");
    motor_manager_create_driver(&motor_manager, "left-shoulder");
    motor_manager_create_driver(&motor_manager, "left-elbow");

    motor_manager_create_driver(&motor_manager, "right-z");
    motor_manager_create_driver(&motor_manager, "right-shoulder");
    motor_manager_create_driver(&motor_manager, "right-elbow");
}

void init_hands(void)
{
    motor_manager_create_driver(&motor_manager, "left-wrist");
    motor_manager_create_driver(&motor_manager, "right-wrist");

    motor_manager_create_driver(&motor_manager, "right-element-rotate");

    // bus_enumerator_add_node(&bus_enumerator, "left-hand", NULL);
    bus_enumerator_add_node(&bus_enumerator, "right-hand", NULL);
}

void __stack_chk_fail(void)
{
    chSysHalt("Stack smashing detected");
}

void context_switch_hook(void *ntp, void *otp)
{
    (void) otp;

    /* The main thread does not have the same memory layout as the other ones
       (it uses the process stack instead of its own stack), so we ignore it. */
    if (ntp == &ch.mainthread) {
        return;
    }

    /* Note: We want to use mpu_configure_region inside a thread
       or an ISR context. It turns out ChibiOS doesn't like it (panic)
       if you lock around mpu_configure_region in here. */
    mpu_configure_region(6,
                         /* we skip sizeof(thread_t) because the start of the working area is used by ChibiOS. */
                         ntp + sizeof(thread_t) + 32,
                         5, /* 32 bytes */
                         AP_NO_NO, /* no permission */
                         false);

    const char *name = ((thread_t *)ntp)->p_name;
    if (name == NULL) {
        name = "no name";
    }
    trace_string(TRACE_POINT_CONTEXT_SWITCH, name);
}
