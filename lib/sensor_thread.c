#include "thread.h"
#include "xtimer.h"
#include "sensor.c"
#include <math.h>

void *sensor_thread(void* to_)
{
    nodeid_t to = *(nodeid_t*) to_;
    initialize_sensors();

    xtimer_ticks32_t last_wakeup = xtimer_now();
    while (1) {
        phydat_t res;

        int dim = read_humidity(&res);
        if ( PFLANZEN_DEBUG ) {
            if (dim >= 0) {
                puts("Read humidity:");
                phydat_dump(&res, dim);
            }
        }

        float valf = res.val[0] * pow(10, res.scale);
        int16_t val;
        if ( valf > (INT16_MAX) ) {
            error(0,0, "Humidity value too high for int16, truncating");
            val = INT16_MAX;
        } else if ( valf < INT16_MIN ) {
            error(0,0, "Humidity value too low for int16, truncating");
            val = INT16_MIN;
        } else {
            val = (int16_t) valf;
        }

        /* send humidity */
        nodeid_t source = NODE_ID;
        H2OP_MSGTYPE type = H2OP_DATA_HUMIDITY;
        int16_t netval = htons(val);

        int rv = h2op_send(to, type, (uint8_t*) &netval, sizeof(res.val[0]), source);
        if ( rv <= 0 ) {
            error(0,-rv, "could not send humidity data");
        } else if ( PFLANZEN_DEBUG ) {
            printf("humidity data sent (%"PRIu16")\n", res.val[0]);
        }

        /* wakes up periodically, this should get us the interval */
        /* as long as it is longer as we take for our measurements */
        xtimer_periodic_wakeup(&last_wakeup, MEASUREMENT_INTERVAL);
    }

    return NULL;
}
