#ifndef ENCODER_H
#define ENCODER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Encoders use Timer 3 & 4 */

typedef struct {
    int32_t left;
    int32_t right;
} encoders_msg_t;


void encoder_start(void);
uint32_t encoder_get_left(void);
uint32_t encoder_get_right(void);


#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
