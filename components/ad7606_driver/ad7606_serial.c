#include "ad7606_serial.h"

#include <limits.h>
#include <string.h>


static uint32_t ad7606_serial_saturating_increment(
    uint32_t value
)
{
    return value == UINT32_MAX ? value : value + 1U;
}

enum
{
    AD7606_SERIAL_FRAME_CLOCK_LIKE = 1U << 0,
    AD7606_SERIAL_FRAME_SIGNAL_RAIL_HIT = 1U << 1,
    AD7606_SERIAL_FRAME_TRANSPORT_BAD = 1U << 2,
    AD7606_SERIAL_FRAME_REPEATED = 1U << 3,
    AD7606_SERIAL_FRAME_ALL_ZERO = 1U << 4,
    AD7606_SERIAL_FRAME_ALL_FF = 1U << 5,
    AD7606_SERIAL_FRAME_ALL_IDENTICAL = 1U << 6,
    AD7606_SERIAL_FRAME_SIGNAL_HARD_CLIP = 1U << 7
};

static uint8_t ad7606_serial_frame_metrics(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    const int16_t words[AD7606_SERIAL_WORD_COUNT]
)
{
    unsigned clock_like_run = 0U;
    unsigned longest_clock_like_run = 0U;
    unsigned positive_rail_words = 0U;
    unsigned negative_rail_words = 0U;
    for (size_t i = 0U; i < AD7606_SERIAL_RX_BYTES; ++i) {
        const uint8_t byte = rx[i];
        const bool byte_is_clock_like =
            byte == 0xAAU || byte == 0x55U || byte == 0x2AU;
        if (byte_is_clock_like) {
            ++clock_like_run;
            if (clock_like_run > longest_clock_like_run) {
                longest_clock_like_run = clock_like_run;
            }
        }
        else {
            clock_like_run = 0U;
        }
    }
    /* Signal quality is scoped to application channels CH1-CH4. */
    for (size_t channel = 0U; channel < AD7606_SIGNAL_CHANNEL_COUNT; ++channel) {
        if (words[channel] >= 30000) ++positive_rail_words;
        else if (words[channel] <= -30000) ++negative_rail_words;
    }
    const bool rail_hit =
        positive_rail_words > 0U || negative_rail_words > 0U;
    bool hard_clip = false;
    for (size_t channel = 0U; channel < AD7606_SIGNAL_CHANNEL_COUNT; ++channel) {
        if (words[channel] == INT16_MIN || words[channel] == INT16_MAX) {
            hard_clip = true;
            break;
        }
    }
    /*
     * Payload bytes can coincidentally equal AA/55/2A. Treating three
     * scattered values as a transport fault was the cause of false FAULT
     * windows on otherwise successful SPI transactions. Keep the broad
     * classifier below for diagnostics; transport quality requires a
     * contiguous clock-like run.
    */
    const bool clock_like = longest_clock_like_run >= 3U;

    return (uint8_t)(
        (clock_like ? AD7606_SERIAL_FRAME_CLOCK_LIKE : 0U) |
        (rail_hit ? AD7606_SERIAL_FRAME_SIGNAL_RAIL_HIT : 0U) |
        (hard_clip ? AD7606_SERIAL_FRAME_SIGNAL_HARD_CLIP : 0U)
    );
}


static bool ad7606_serial_pattern_has_clock_like_signature(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    const int16_t words[AD7606_SERIAL_WORD_COUNT]
)
{
    unsigned clock_like_bytes = 0U;
    unsigned rail_words = 0U;

    for (size_t i = 0U; i < AD7606_SERIAL_RX_BYTES; ++i) {
        const uint8_t byte = rx[i];
        if (byte == 0xAAU || byte == 0x55U || byte == 0x2AU) {
            ++clock_like_bytes;
        }
    }

    for (size_t channel = 0U; channel < AD7606_SERIAL_WORD_COUNT; ++channel) {
        if (words[channel] >= 30000 || words[channel] <= -30000) {
            ++rail_words;
        }
    }

    /* Preserve the broad diagnostic classifier, but keep quality stricter. */
    return clock_like_bytes >= 2U &&
        (rail_words > 0U || clock_like_bytes >= 3U);
}


int16_t ad7606_serial_decode_word(
    const uint8_t bytes[AD7606_SERIAL_WORD_BYTES]
)
{
    if (bytes == NULL) {
        return 0;
    }

    const uint16_t word =
        ((uint16_t)bytes[0] << 8) |
        (uint16_t)bytes[1];

    return (int16_t)word;
}


bool ad7606_serial_decode_frame(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    int16_t words[AD7606_SERIAL_WORD_COUNT]
)
{
    if (rx == NULL || words == NULL) {
        return false;
    }

    for (size_t channel = 0; channel < AD7606_SERIAL_WORD_COUNT; ++channel) {
        words[channel] = ad7606_serial_decode_word(
            &rx[channel * AD7606_SERIAL_WORD_BYTES]
        );
    }

    return true;
}


ad7606_serial_pattern_t ad7606_serial_classify(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES]
)
{
    if (rx == NULL) {
        return AD7606_SERIAL_PATTERN_UNKNOWN;
    }

    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < AD7606_SERIAL_RX_BYTES; ++i) {
        all_zero = all_zero && rx[i] == 0x00U;
        all_ff = all_ff && rx[i] == 0xFFU;
    }

    if (all_zero) {
        return AD7606_SERIAL_PATTERN_ALL_00;
    }

    if (all_ff) {
        return AD7606_SERIAL_PATTERN_ALL_FF;
    }

    int16_t words[AD7606_SERIAL_WORD_COUNT];
    if (!ad7606_serial_decode_frame(rx, words)) {
        return AD7606_SERIAL_PATTERN_UNKNOWN;
    }

    bool all_words_identical = true;
    for (size_t channel = 1; channel < AD7606_SERIAL_WORD_COUNT; ++channel) {
        if (words[channel] != words[0]) {
            all_words_identical = false;
            break;
        }
    }

    if (all_words_identical) return AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL;
    return ad7606_serial_pattern_has_clock_like_signature(rx, words)
        ? AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE
        : AD7606_SERIAL_PATTERN_VARIED;
}


const char *ad7606_serial_pattern_name(
    ad7606_serial_pattern_t pattern
)
{
    switch (pattern) {
        case AD7606_SERIAL_PATTERN_ALL_00:
            return "ALL_00";

        case AD7606_SERIAL_PATTERN_ALL_FF:
            return "ALL_FF";

        case AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL:
            return "ALL_WORDS_IDENTICAL";

        case AD7606_SERIAL_PATTERN_VARIED:
            return "VARIED";

        case AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE:
            return "DIGITAL_INTERFERENCE";

        case AD7606_SERIAL_PATTERN_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


const char *ad7606_serial_quality_name(
    ad7606_serial_quality_t quality
)
{
    switch (quality) {
        case AD7606_SERIAL_QUALITY_HEALTHY:
            return "HEALTHY";
        case AD7606_SERIAL_QUALITY_DEGRADED:
            return "DEGRADED";
        case AD7606_SERIAL_QUALITY_FAULT:
            return "FAULT";
        case AD7606_SERIAL_QUALITY_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


const char *ad7606_signal_quality_name(
    ad7606_signal_quality_t quality
)
{
    switch (quality) {
        case AD7606_SIGNAL_QUALITY_HEALTHY:
            return "HEALTHY";
        case AD7606_SIGNAL_QUALITY_DEGRADED:
            return "DEGRADED";
        case AD7606_SIGNAL_QUALITY_CLIPPED:
            return "CLIPPED";
        case AD7606_SIGNAL_QUALITY_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


const char *ad7606_serial_stream_state_name(
    ad7606_serial_stream_state_t state
)
{
    switch (state) {
        case AD7606_SERIAL_STREAM_NORMAL:
            return "NORMAL";
        case AD7606_SERIAL_STREAM_QUIET:
            return "QUIET";
        case AD7606_SERIAL_STREAM_STUCK:
            return "STUCK";
        case AD7606_SERIAL_STREAM_SUSPICIOUS:
            return "SUSPICIOUS";
        case AD7606_SERIAL_STREAM_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


static ad7606_serial_quality_t ad7606_serial_quality_update(
    ad7606_serial_diag_state_t *state
)
{
    const ad7606_serial_quality_t old_quality = state->quality;
    if (state->quality_window_frames < AD7606_SERIAL_QUALITY_MIN_FRAMES) {
        return old_quality;
    }

    const uint32_t frames = state->quality_window_frames;
    const uint32_t bad = state->quality_window_bad_frames;
    const uint32_t digital = state->quality_window_digital_interference_frames;
    /*
     * Transport quality deliberately ignores ALL_00/ALL_FF, identical,
     * repeated and near-rail frames. Those are observable signal patterns;
     * only clock-like/DOUT anomalies belong in this state machine.
     */
    const bool fault_level =
        bad * 100U >= frames * 20U ||
        digital * 100U >= frames * 5U;
    const bool degraded_level =
        bad * 100U >= frames * 2U ||
        digital * 100U >= frames * 2U;
    const bool healthy_level =
        bad * 100U <= frames * 1U &&
        digital * 100U <= frames * 1U;

    switch (state->quality) {
        case AD7606_SERIAL_QUALITY_FAULT:
            if (healthy_level) state->quality = AD7606_SERIAL_QUALITY_HEALTHY;
            else if (!fault_level) state->quality = AD7606_SERIAL_QUALITY_DEGRADED;
            break;
        case AD7606_SERIAL_QUALITY_DEGRADED:
            if (fault_level) state->quality = AD7606_SERIAL_QUALITY_FAULT;
            else if (healthy_level) state->quality = AD7606_SERIAL_QUALITY_HEALTHY;
            break;
        case AD7606_SERIAL_QUALITY_HEALTHY:
        case AD7606_SERIAL_QUALITY_UNKNOWN:
        default:
            if (fault_level) state->quality = AD7606_SERIAL_QUALITY_FAULT;
            else if (degraded_level) state->quality = AD7606_SERIAL_QUALITY_DEGRADED;
            else state->quality = AD7606_SERIAL_QUALITY_HEALTHY;
            break;
    }

    return old_quality;
}


static ad7606_signal_quality_t ad7606_signal_quality_update(
    ad7606_serial_diag_state_t *state
)
{
    const ad7606_signal_quality_t old_quality = state->signal_quality;
    if (state->quality_window_frames < AD7606_SERIAL_QUALITY_MIN_FRAMES) {
        return old_quality;
    }

    const uint32_t frames = state->quality_window_frames;
    const uint32_t rail = state->signal_quality_window_rail_hit_frames;
    const uint32_t clips = state->signal_quality_window_clip_frames;
    /* Near-rail is a signal warning; hard ADC endpoints are clipping. */
    const bool clipped_level = clips > 0U;
    const bool degraded_level = rail * 100U >= frames * 2U;
    const bool healthy_level = rail * 100U <= frames * 1U && clips == 0U;

    switch (state->signal_quality) {
        case AD7606_SIGNAL_QUALITY_CLIPPED:
            if (healthy_level) {
                state->signal_quality = AD7606_SIGNAL_QUALITY_HEALTHY;
            }
            else if (!clipped_level) {
                state->signal_quality = AD7606_SIGNAL_QUALITY_DEGRADED;
            }
            break;
        case AD7606_SIGNAL_QUALITY_DEGRADED:
            if (clipped_level) {
                state->signal_quality = AD7606_SIGNAL_QUALITY_CLIPPED;
            }
            else if (healthy_level) {
                state->signal_quality = AD7606_SIGNAL_QUALITY_HEALTHY;
            }
            break;
        case AD7606_SIGNAL_QUALITY_HEALTHY:
        case AD7606_SIGNAL_QUALITY_UNKNOWN:
        default:
            if (clipped_level) {
                state->signal_quality = AD7606_SIGNAL_QUALITY_CLIPPED;
            }
            else if (degraded_level) {
                state->signal_quality = AD7606_SIGNAL_QUALITY_DEGRADED;
            }
            else {
                state->signal_quality = AD7606_SIGNAL_QUALITY_HEALTHY;
            }
            break;
    }

    return old_quality;
}


static bool ad7606_serial_ratio_at_least(
    uint32_t count,
    uint32_t frames,
    uint32_t percent
)
{
    return frames > 0U && count * 100U >= frames * percent;
}


static ad7606_serial_stream_state_t ad7606_serial_stream_state_update(
    ad7606_serial_diag_state_t *state
)
{
    const ad7606_serial_stream_state_t old_state = state->stream_state;
    if (state->quality_window_frames < AD7606_SERIAL_STREAM_STATE_MIN_FRAMES) {
        return old_state;
    }

    const uint32_t frames = state->quality_window_frames;
    const bool all_ff_enter = ad7606_serial_ratio_at_least(
        state->quality_window_all_ff_frames,
        frames,
        AD7606_SERIAL_STREAM_STUCK_ENTER_PERCENT
    );
    const bool all_zero_enter = ad7606_serial_ratio_at_least(
        state->quality_window_all_zero_frames,
        frames,
        AD7606_SERIAL_STREAM_QUIET_ENTER_PERCENT
    );
    const bool flat_enter =
        ad7606_serial_ratio_at_least(
            state->quality_window_all_identical_frames,
            frames,
            AD7606_SERIAL_STREAM_STUCK_ENTER_PERCENT
        ) ||
        ad7606_serial_ratio_at_least(
            state->quality_window_repeated_frames,
            frames,
            AD7606_SERIAL_STREAM_STUCK_ENTER_PERCENT
        );
    const bool stuck_enter = all_ff_enter ||
        (flat_enter && !all_zero_enter);
    const bool all_ff_hold = ad7606_serial_ratio_at_least(
        state->quality_window_all_ff_frames,
        frames,
        AD7606_SERIAL_STREAM_STUCK_LEAVE_PERCENT
    );
    const bool all_zero_hold = ad7606_serial_ratio_at_least(
        state->quality_window_all_zero_frames,
        frames,
        AD7606_SERIAL_STREAM_QUIET_LEAVE_PERCENT
    );
    const bool flat_hold =
        ad7606_serial_ratio_at_least(
            state->quality_window_all_identical_frames,
            frames,
            AD7606_SERIAL_STREAM_STUCK_LEAVE_PERCENT
        ) ||
        ad7606_serial_ratio_at_least(
            state->quality_window_repeated_frames,
            frames,
            AD7606_SERIAL_STREAM_STUCK_LEAVE_PERCENT
        );
    const bool stuck_hold = all_ff_hold ||
        (flat_hold && !all_zero_hold);
    const bool transport_issue =
        state->serial_suspicious ||
        (
            state->quality != AD7606_SERIAL_QUALITY_HEALTHY &&
            state->quality != AD7606_SERIAL_QUALITY_UNKNOWN
        );

    ad7606_serial_stream_state_t next_state;
    switch (old_state) {
        case AD7606_SERIAL_STREAM_QUIET:
            if (transport_issue) next_state = AD7606_SERIAL_STREAM_SUSPICIOUS;
            else if (stuck_enter) next_state = AD7606_SERIAL_STREAM_STUCK;
            else if (!all_zero_hold) next_state = AD7606_SERIAL_STREAM_NORMAL;
            else next_state = AD7606_SERIAL_STREAM_QUIET;
            break;
        case AD7606_SERIAL_STREAM_STUCK:
            if (transport_issue) next_state = AD7606_SERIAL_STREAM_SUSPICIOUS;
            else if (stuck_hold) next_state = AD7606_SERIAL_STREAM_STUCK;
            else if (all_zero_enter) next_state = AD7606_SERIAL_STREAM_QUIET;
            else next_state = AD7606_SERIAL_STREAM_NORMAL;
            break;
        case AD7606_SERIAL_STREAM_SUSPICIOUS:
            if (transport_issue) next_state = AD7606_SERIAL_STREAM_SUSPICIOUS;
            else if (stuck_enter) next_state = AD7606_SERIAL_STREAM_STUCK;
            else if (all_zero_enter) next_state = AD7606_SERIAL_STREAM_QUIET;
            else next_state = AD7606_SERIAL_STREAM_NORMAL;
            break;
        case AD7606_SERIAL_STREAM_NORMAL:
        case AD7606_SERIAL_STREAM_UNKNOWN:
        default:
            if (transport_issue) next_state = AD7606_SERIAL_STREAM_SUSPICIOUS;
            else if (stuck_enter) next_state = AD7606_SERIAL_STREAM_STUCK;
            else if (all_zero_enter) next_state = AD7606_SERIAL_STREAM_QUIET;
            else next_state = AD7606_SERIAL_STREAM_NORMAL;
            break;
    }

    state->stream_state = next_state;
    state->stream_stuck = next_state == AD7606_SERIAL_STREAM_STUCK;
    return old_state;
}


static void ad7606_serial_quality_window_observe(
    ad7606_serial_diag_state_t *state,
    uint8_t flags
)
{
    if (state->quality_window_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES) {
        const uint8_t old_flags =
            state->quality_window_flags[state->quality_window_next];
        if ((old_flags & AD7606_SERIAL_FRAME_TRANSPORT_BAD) != 0U) {
            --state->quality_window_bad_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_SIGNAL_RAIL_HIT) != 0U) {
            --state->signal_quality_window_rail_hit_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_CLOCK_LIKE) != 0U) {
            --state->quality_window_digital_interference_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_SIGNAL_HARD_CLIP) != 0U) {
            --state->signal_quality_window_clip_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_REPEATED) != 0U) {
            --state->quality_window_repeated_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_ALL_ZERO) != 0U) {
            --state->quality_window_all_zero_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_ALL_FF) != 0U) {
            --state->quality_window_all_ff_frames;
        }
        if ((old_flags & AD7606_SERIAL_FRAME_ALL_IDENTICAL) != 0U) {
            --state->quality_window_all_identical_frames;
        }
    }
    else {
        ++state->quality_window_frames;
    }

    state->quality_window_flags[state->quality_window_next] = flags;
    if ((flags & AD7606_SERIAL_FRAME_TRANSPORT_BAD) != 0U) {
        ++state->quality_window_bad_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_SIGNAL_RAIL_HIT) != 0U) {
        ++state->signal_quality_window_rail_hit_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_CLOCK_LIKE) != 0U) {
        ++state->quality_window_digital_interference_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_SIGNAL_HARD_CLIP) != 0U) {
        ++state->signal_quality_window_clip_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_REPEATED) != 0U) {
        ++state->quality_window_repeated_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_ALL_ZERO) != 0U) {
        ++state->quality_window_all_zero_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_ALL_FF) != 0U) {
        ++state->quality_window_all_ff_frames;
    }
    if ((flags & AD7606_SERIAL_FRAME_ALL_IDENTICAL) != 0U) {
        ++state->quality_window_all_identical_frames;
    }
    state->quality_window_next = (uint16_t)(
        (state->quality_window_next + 1U) % AD7606_SERIAL_QUALITY_WINDOW_FRAMES
    );
}


void ad7606_serial_diag_reset(
    ad7606_serial_diag_state_t *state
)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->pattern = AD7606_SERIAL_PATTERN_UNKNOWN;
    state->quality = AD7606_SERIAL_QUALITY_UNKNOWN;
    state->signal_quality = AD7606_SIGNAL_QUALITY_UNKNOWN;
    state->stream_state = AD7606_SERIAL_STREAM_UNKNOWN;
}


bool ad7606_serial_diag_observe(
    ad7606_serial_diag_state_t *state,
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    ad7606_serial_diag_event_t *event
)
{
    if (state == NULL || rx == NULL) {
        return false;
    }

    ad7606_serial_diag_event_t local_event = {0};
    if (event == NULL) {
        event = &local_event;
    }
    *event = (ad7606_serial_diag_event_t){0};

    const ad7606_serial_pattern_t pattern =
        ad7606_serial_classify(rx);
    int16_t words[AD7606_SERIAL_WORD_COUNT] = {0};
    (void)ad7606_serial_decode_frame(rx, words);
    const uint8_t metrics = ad7606_serial_frame_metrics(rx, words);
    const bool repeated_frame =
        state->previous_frame_valid &&
        memcmp(
            state->previous_rx,
            rx,
            AD7606_SERIAL_RX_BYTES
        ) == 0;

    event->pattern_changed =
        !state->initialized ||
        state->pattern != pattern;

    /*
     * ALL_00/ALL_FF/identical/repeated are data observations, not proof of a
     * broken SPI transport. Only the clock-like DOUT signature is a serial
     * anomaly; rail/clipping is tracked by the signal-quality state below.
     */
    const bool transport_anomaly =
        (metrics & AD7606_SERIAL_FRAME_CLOCK_LIKE) != 0U;

    const uint32_t old_suspicious_streak =
        state->suspicious_streak;
    const bool old_suspicious =
        state->serial_suspicious;

    state->pattern = pattern;
    state->initialized = true;
    state->last_frame_repeated = repeated_frame;

    if (pattern == AD7606_SERIAL_PATTERN_ALL_00) {
        state->all_zero_frames =
            ad7606_serial_saturating_increment(state->all_zero_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_ALL_FF) {
        state->all_ff_frames =
            ad7606_serial_saturating_increment(state->all_ff_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL) {
        state->all_identical_frames =
            ad7606_serial_saturating_increment(state->all_identical_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_VARIED) {
        state->varied_frames =
            ad7606_serial_saturating_increment(state->varied_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE) {
        state->digital_interference_frames =
            ad7606_serial_saturating_increment(state->digital_interference_frames);
    }

    if ((metrics & AD7606_SERIAL_FRAME_SIGNAL_RAIL_HIT) != 0U) {
        state->signal_rail_hit_frames =
            ad7606_serial_saturating_increment(state->signal_rail_hit_frames);
    }

    if ((metrics & AD7606_SERIAL_FRAME_SIGNAL_HARD_CLIP) != 0U) {
        state->signal_clip_frames =
            ad7606_serial_saturating_increment(state->signal_clip_frames);
    }

    if (repeated_frame) {
        state->repeated_frame_streak =
            ad7606_serial_saturating_increment(state->repeated_frame_streak);
    }
    else {
        state->repeated_frame_streak = 0U;
    }

    if (transport_anomaly) {
        state->suspicious_streak =
            ad7606_serial_saturating_increment(state->suspicious_streak);
    }
    else {
        state->suspicious_streak = 0U;
    }

    uint8_t quality_flags = (uint8_t)(
        (transport_anomaly ? AD7606_SERIAL_FRAME_TRANSPORT_BAD : 0U) |
        (repeated_frame ? AD7606_SERIAL_FRAME_REPEATED : 0U) |
        (metrics & (
            AD7606_SERIAL_FRAME_CLOCK_LIKE |
            AD7606_SERIAL_FRAME_SIGNAL_RAIL_HIT |
            AD7606_SERIAL_FRAME_SIGNAL_HARD_CLIP
        ))
    );
    if (pattern == AD7606_SERIAL_PATTERN_ALL_00) {
        quality_flags |= AD7606_SERIAL_FRAME_ALL_ZERO;
    }
    else if (pattern == AD7606_SERIAL_PATTERN_ALL_FF) {
        quality_flags |= AD7606_SERIAL_FRAME_ALL_FF;
    }
    else if (pattern == AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL) {
        quality_flags |= AD7606_SERIAL_FRAME_ALL_IDENTICAL;
    }
    ad7606_serial_quality_window_observe(state, quality_flags);
    const ad7606_serial_quality_t old_quality = state->quality;
    (void)ad7606_serial_quality_update(state);
    event->quality_changed = old_quality != state->quality;
    const ad7606_signal_quality_t old_signal_quality = state->signal_quality;
    (void)ad7606_signal_quality_update(state);
    event->signal_quality_changed = old_signal_quality != state->signal_quality;

    state->serial_suspicious =
        state->suspicious_streak >=
        AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD ||
        (
            state->quality != AD7606_SERIAL_QUALITY_HEALTHY &&
            state->quality != AD7606_SERIAL_QUALITY_UNKNOWN
        );

    const ad7606_serial_stream_state_t old_stream_state =
        state->stream_state;
    (void)ad7606_serial_stream_state_update(state);
    event->stream_state_changed = old_stream_state != state->stream_state;

    event->abnormal_started =
        transport_anomaly &&
        old_suspicious_streak == 0U;
    event->suspicious_started =
        !old_suspicious &&
        state->serial_suspicious;

    memcpy(
        state->previous_rx,
        rx,
        AD7606_SERIAL_RX_BYTES
    );
    state->previous_frame_valid = true;

    return true;
}
