#include <stdio.h>
#include <string.h>

#include <msg.h>
#include <shell.h>
#include <random.h>

#include "lib/network.c"
#include "lib/pump.c"
#include "lib/sensor.c"
#include "lib/sensor_thread.c"
#include "lib/util.c"
#include "lib/global.c"

/* stack containing all started stacks */
char thread_stack[THREAD_STACKSIZE_MAIN];

#define MAIN_QUEUE_SIZE     (8)
static msg_t _main_msg_queue[MAIN_QUEUE_SIZE];

static const shell_command_t shell_commands[] = {
    { "light", "read light data", read_light_shell },
    { "humidity", "read humidity data", read_humidity_shell },
    { "h2od", "start h2o server", shell_h2od },
    { "debug", "turn debug prints on and off", shell_debug },
    { "pump_set_data", "Send data to the pump controller", shell_pump_set_data },
    { "h2o_send_data", "send data using the h2o protocol", h2o_send_data_shell },
    { "h2o_miv", "Set measurement interval", h2o_set_measurement_interval_shell },
    { "info", "Print information about the node", shell_info },
    { "exit", "Terminate program", shell_exit },
    { NULL, NULL, NULL },
};


int main(void)
{
    // NODE_ID declared in global.c
#if defined(NODE_ID_RANDOM)
    // We reserve 0xff.. for special addresses
    if ( PFLANZEN_DEBUG) {
        puts("Setting random node id...");
    }
    NODE_ID = (nodeid_t) random_uint32_range(1, 0xff00);
#elif defined(NODE_ID_DEVICE)
    NODE_ID = nodeid_from_device();
    if ( NODE_ID == 0 ) {
        error(0,0,"Could not set node id from device");
        return 0;
    }
#else
    NODE_ID = (NODE_ID_);
#endif

    msg_init_queue(_main_msg_queue, MAIN_QUEUE_SIZE);
    printf("[Pflanzen 1] Welcome! I am a %s, my node ID is %04X.\n",
           NODE_ROLE, NODE_ID);

#ifdef NODE_ROLE_SENSOR
    network_init(false);
#endif
#ifdef NODE_ROLE_COLLECTOR
    network_init(true);
#endif

// make sure this is the first hook so first thing we do is print received data
h2op_add_receive_hook(&h2op_debug_hook);
#ifdef UPSTREAM_NODE
    h2op_add_receive_hook(h2op_forward_data_hook);
#endif
#ifdef NODE_ROLE_COLLECTOR
    h2op_add_receive_hook(h2op_pump_set_data_hook);
#endif
#if defined(NODE_ROLE_SENSOR) || defined(NODE_ROLE_COLLECTOR)
    h2op_add_receive_hook(h2op_measurement_interval_hook);
#endif
    //TODO maybe we don't always need it?
    h2od_start();

    initialize_sensors();

#if defined(NODE_ROLE_COLLECTOR) || defined(NODE_ROLE_SENSOR)
#ifdef NODE_ROLE_COLLECTOR
    // send to ourselves (we need this for the water level check)
    nodeid_t to = COLLECTOR_NODE_ID;
#endif
#ifdef NODE_ROLE_SENSOR
    nodeid_t to = UPSTREAM_NODE;
#endif
    thread_create(thread_stack, sizeof(thread_stack),
                  THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST,
                  sensor_thread, (void*) &to, "sensor_thread");
#endif

    char line_buf[SHELL_DEFAULT_BUFSIZE];
    shell_run(shell_commands, line_buf, SHELL_DEFAULT_BUFSIZE);

    return 0;
}
