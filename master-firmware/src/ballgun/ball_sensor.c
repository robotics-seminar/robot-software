#include <string.h>

#include "ball_sensor.h"

enum ball_sensor_state_t ball_sensor_state(bool previous, bool current)
{
    if (previous == true) {
        return (current == true) ? BALL_SENSOR_HIGH : BALL_SENSOR_FALLING;
    } else /* previous == false */ {
        return (current == false) ? BALL_SENSOR_LOW : BALL_SENSOR_RISING;
    }
}

void ball_sensor_init(ball_sensor_t* sensor)
{
    memset(sensor, 0, sizeof(ball_sensor_t));
}

void ball_sensor_update(ball_sensor_t* sensor, bool measurement)
{
    enum ball_sensor_state_t state = ball_sensor_state(sensor->previous, measurement);
    if (state == BALL_SENSOR_RISING) {
        sensor->high_count = 0;
    }
    if (state == BALL_SENSOR_FALLING) {
        sensor->low_count = 0;
    }

    if (measurement == true) {
        sensor->high_count++;
    } else {
        sensor->low_count++;
    }

    sensor->previous = measurement;
}

bool ball_sensor_detect_pulse(ball_sensor_t* sensor, unsigned low_threshold, unsigned high_threshold)
{
    if (sensor->high_count == high_threshold && sensor->low_count >= low_threshold) {
        return true;
    }
    return false;
}

void ball_sensor_manage(ball_sensor_t* sensor, bool measurement)
{
    ball_sensor_update(sensor, measurement);
    if (ball_sensor_detect_pulse(sensor, sensor->low_threshold, sensor->high_threshold)) {
        if (measurement == sensor->ball_detection_signal) {
            sensor->ball_count++;
        }
    }
}

void ball_sensor_configure(ball_sensor_t *sensor, bool ball_detection_signal,
                           unsigned low_threshold, unsigned high_threshold)
{
    sensor->ball_detection_signal = ball_detection_signal;
    sensor->low_threshold = low_threshold;
    sensor->high_threshold = high_threshold;
}
