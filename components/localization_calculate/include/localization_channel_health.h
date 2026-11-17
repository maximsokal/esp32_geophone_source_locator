#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Is every geophone channel really an independent sensor? Two faults make a source impossible to locate and look like
 * "no source found": two channels carrying the same signal (a short between inputs or amplifier outputs, one geophone
 * wired to two inputs), and a channel that stays flat while its neighbours move (a sensor that is not connected).
 * Board logs 2026-11-14/15: channels 2 and 3 equal to within a few ADC codes at a 4000-code swing, channels 1 and 4 flat.
 *
 * The check runs on the recording's analysis window, in microvolts, on the channels that carry a sensor.
 */
#define LOCALIZATION_CHANNEL_HEALTH_CHANNELS 4U
#define LOCALIZATION_CHANNEL_HEALTH_PAIRS 6U

/* A channel needs at least this much signal (RMS about its mean) to be judged against another one: 20 mV is ~130 ADC
 * codes, far above the converter's own noise of a few codes. */
#define LOCALIZATION_CHANNEL_HEALTH_ACTIVE_RMS_UV 20000.0f
/* Two active channels are "the same signal" when what is left after subtracting one from the other is at most this share
 * of the weaker one. Independent sensors never get this close; the noise of two inputs adds ~3 % at the active floor. */
#define LOCALIZATION_CHANNEL_HEALTH_IDENTICAL_RESIDUAL 0.06f
/* A channel is flat when its largest excursion from its mean stays under this, 4 mV (~26 ADC codes), while the strongest
 * other channel exceeds LOCALIZATION_CHANNEL_HEALTH_FLAT_NEIGHBOUR_RATIO times that and the active floor. */
#define LOCALIZATION_CHANNEL_HEALTH_FLAT_PEAK_UV 4000.0f
#define LOCALIZATION_CHANNEL_HEALTH_FLAT_NEIGHBOUR_RATIO 20.0f

typedef struct
{
    /* Bit c: channel c is flat although another channel is active. */
    uint8_t flat_channel_mask;
    /* Bit p (see localization_channel_health_pair): the two channels of pair p carry the same signal. */
    uint8_t identical_pair_mask;

} localization_channel_health_t;

/* Pair p as two channel indices (a < b): 0:(0,1) 1:(0,2) 2:(0,3) 3:(1,2) 4:(1,3) 5:(2,3). False when p is out of range. */
int localization_channel_health_pair(size_t pair, size_t *channel_a, size_t *channel_b);

/*
 * channel_uv[c] points to `count` samples (microvolts) of channel c, or is NULL for a channel without a sensor: such a
 * channel is neither judged nor used as a reference. With fewer than two sensors nothing can be compared and the result
 * is all clear.
 */
void localization_channel_health_analyze(
    const float *const channel_uv[LOCALIZATION_CHANNEL_HEALTH_CHANNELS],
    size_t count,
    localization_channel_health_t *health_out
);

/*
 * The same judgement on the recording as it is stored: frames of four interleaved channels, `stride` floats from one
 * frame's sample of a channel to the next frame's (first_sample points at channel 0 of frame 0). Every channel has data.
 *
 * `sensor_mask` (bit c: channel c carries a sensor) limits which channels can be called flat, but identical pairs are
 * judged among ALL four channels. Board card 2026-11-16: from session S000005 on, AD7606 channels 1 and 2 carried one
 * signal (correlation 0.99999) while the node of channel 1 was not assigned; the check that skipped channels without a
 * sensor never saw the pair, and the device reported only "channels 0 and 3 flat". A copy of a sensor's signal on a channel
 * with no sensor is the same wiring fault, and a more telling one.
 */
void localization_channel_health_analyze_interleaved(
    const float *first_sample,
    size_t stride,
    size_t count,
    uint8_t sensor_mask,
    localization_channel_health_t *health_out
);

#ifdef __cplusplus
}
#endif
