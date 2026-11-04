#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "nrf_cs_iq_gap_fill.h"
#include "nrf_cs_ifft_quality.h"
#include "nrf_cs_measurement_quality.h"
#include "bluetooth/cs_de.h"
typedef int arm_cfft_instance_f32;
#if CONFIG_BT_CS_DE_NFFT_SIZE == 512
static const int arm_cfft_sR_f32_len512=0;
#elif CONFIG_BT_CS_DE_NFFT_SIZE == 1024
static const int arm_cfft_sR_f32_len1024=0;
#elif CONFIG_BT_CS_DE_NFFT_SIZE == 2048
static const int arm_cfft_sR_f32_len2048=0;
#endif
static void arm_sqrt_f32(float x,float *out){*out=sqrtf(x);}
static void arm_max_f32(const float *a,uint32_t n,float *out,uint32_t *idx){*out=a[0];*idx=0;for(uint32_t i=1;i<n;i++)if(a[i]>*out){*out=a[i];*idx=i;}}
static void arm_cfft_f32(const arm_cfft_instance_f32 *inst,float *a,int inv,int reverse)
{
 (void)inst;(void)inv;(void)reverse;unsigned n=CONFIG_BT_CS_DE_NFFT_SIZE;
 for(unsigned i=1,j=0;i<n;i++){unsigned bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j){float x=a[2*i],y=a[2*i+1];a[2*i]=a[2*j];a[2*i+1]=a[2*j+1];a[2*j]=x;a[2*j+1]=y;}}
 for(unsigned len=2;len<=n;len*=2){float ang=-6.2831853071795864769f/(float)len;float wr0=cosf(ang),wi0=sinf(ang);for(unsigned i=0;i<n;i+=len){float wr=1,wi=0;for(unsigned j=0;j<len/2;j++){unsigned u=2*(i+j),v=2*(i+j+len/2);float vr=a[v]*wr-a[v+1]*wi,vi=a[v]*wi+a[v+1]*wr;float ur=a[u],ui=a[u+1];a[u]=ur+vr;a[u+1]=ui+vi;a[v]=ur-vr;a[v+1]=ui-vi;float t=wr*wr0-wi*wi0;wi=wr*wi0+wi*wr0;wr=t;}}}
}
#include "ble6_sdk_cs_de.c"
#include "nrf_cs_rf_diagnostics.h"
#define NRF_CS_CHANNEL_INDEX_OFFSET 2U
#define BT_HCI_LE_CS_TONE_QUALITY_HIGH 0U
#define BT_HCI_LE_CS_PCT_I_MASK 0xfffU
#define BT_HCI_LE_CS_PCT_Q_MASK 0xfff000U
#define BIT(n) (1U << (n))
struct bt_le_cs_iq_sample { int16_t i, q; };
struct bt_le_cs_subevent_step { uint8_t mode, channel, data_len; const uint8_t *data; };
#pragma pack(push, 1)
struct bt_hci_le_cs_step_data_tone_info {
    uint8_t phase_correction_term[3];
    uint8_t quality_indicator : 4, extension_indicator : 4;
};
struct bt_hci_le_cs_step_data_mode_2 {
    uint8_t antenna_permutation_index;
    struct bt_hci_le_cs_step_data_tone_info tone_info[1];
};
#pragma pack(pop)
static uint16_t s_iq_counts[75];
static nrf_cs_rf_diagnostics_t s_rf_diagnostics;
static uint32_t sys_get_le24(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U); }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#endif
#include "bt_le_cs_parse_pct.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "accumulate_mean.inc"
#include "collect_pbr_step.inc"
int main(void)
{
    /* Nordic 3.4 clamps nonpositive accumulated timing to zero. */
    assert(cs_de_rtt(-1, 1) == 0.0f && cs_de_rtt(0, 1) == 0.0f);
    assert(cs_de_rtt(1, 1) > 0.0f && isnan(cs_de_rtt(0, 0)));
    const char *input = getenv("BLE6_REPLAY_INPUT"), *output = getenv("BLE6_REPLAY_OUTPUT");
    if (input == NULL || output == NULL) return 2;
    FILE *in = fopen(input, "rb"), *out = fopen(output, "wb");
    /* Optional second output: what the production confidence says about the same procedure (4 floats each):
     * IFFT peak over median bin, plain confidence, confidence with the peak, reason code (nrf_cs_confidence_reason_t). */
    const char *extra_path = getenv("BLE6_REPLAY_EXTRA");
    FILE *extra = extra_path != NULL ? fopen(extra_path, "wb") : NULL;
    const char *peak_path = getenv("BLE6_REPLAY_PEAK");
    FILE *peak = peak_path != NULL ? fopen(peak_path, "wb") : NULL;
    if (in == NULL || out == NULL || (extra_path != NULL && extra == NULL) ||
        (peak_path != NULL && peak == NULL)) return 3;
    float src[150], stock[150], buf[2 * CONFIG_BT_CS_DE_NFFT_SIZE], rtt, result[4];
    uint16_t counts[75];
    uint16_t steps;
    size_t procedures = 0U;
    while (fread(&steps, sizeof(steps), 1, in) == 1U) {
        cs_de_report_t report = {0};
        double sums[300] = {0}, products[150] = {0};
        uint16_t observed[75] = {0};
        memset(s_iq_counts, 0, sizeof(s_iq_counts));
        nrf_cs_rf_reset(&s_rf_diagnostics);
        for (uint16_t i = 0; i < steps; ++i) {
            uint8_t raw[11];
            if (fread(raw, sizeof(raw), 1, in) != 1U) return 4;
            const struct bt_le_cs_subevent_step local = {2, raw[0], 5, raw + 1};
            const struct bt_le_cs_subevent_step peer = {2, raw[0], 5, raw + 6};
            collect_pbr_step(&report, &local, &peer);
            if (raw[0] >= 2U && raw[0] <= 76U && (raw[5] & 15U) == 0U && (raw[10] & 15U) == 0U) {
                const size_t k = raw[0] - 2U;
                const struct bt_le_cs_iq_sample a = bt_le_cs_parse_pct(raw + 2);
                const struct bt_le_cs_iq_sample b = bt_le_cs_parse_pct(raw + 7);
                ++observed[k];
                sums[4*k] += a.i; sums[4*k+1] += a.q;
                sums[4*k+2] += b.i; sums[4*k+3] += b.q;
                products[2*k] += (double)a.i*b.i - (double)a.q*b.q;
                products[2*k+1] += (double)a.i*b.q + (double)a.q*b.i;
            }
        }
        if (fread(src, sizeof(src), 1, in) != 1U) return 4;
        if (fread(counts, sizeof(counts), 1, in) != 1U || fread(&rtt, sizeof(rtt), 1, in) != 1U) return 4;
        memcpy(stock, src, sizeof(stock));
        if (steps > 0U) {
            /* Keep the independently reconstructed old product-of-means as the stock control. */
            for (size_t k = 0; k < 75; ++k) {
                assert(counts[k] == observed[k] && counts[k] == s_iq_counts[k]);
                const double n = observed[k];
                buf[2*k] = n > 0 ? (float)((sums[4*k]*sums[4*k+2] - sums[4*k+1]*sums[4*k+3])/(n*n)) : 0.0f;
                buf[2*k+1] = n > 0 ? (float)((sums[4*k]*sums[4*k+3] + sums[4*k+1]*sums[4*k+2])/(n*n)) : 0.0f;
            }
            for (size_t i = 0; i < 150; ++i)
                assert(fabsf(src[i] - buf[i]) <= 0.5f + 0.00001f * fabsf(src[i]));
            cs_de_combined_iq_calculate(&report.iq_tones[0], buf);
            for (size_t i = 0; i < 150; ++i) {
                const float expected = observed[i/2] > 0 ? (float)(products[i]/observed[i/2]) : 0.0f;
                assert(fabsf(expected - buf[i]) <= 0.5f + 0.00001f * fabsf(expected));
                src[i] = buf[i];
            }
        }
        uint16_t good = 0U;
        for (size_t i = 0; i < 75; ++i) good += counts[i] > 0U ? 1U : 0U;
        for (size_t i = 0; i < 2 * CONFIG_BT_CS_DE_NFFT_SIZE; ++i) buf[i] = i < 150 ? src[i] : 0.0f;
        result[2] = good >= 15U ? cs_de_phase_slope(buf) : NAN;
        const float raw_ifft = good >= 15U ? cs_de_ifft(buf) : NAN;
        for (size_t i = 0; i < 2 * CONFIG_BT_CS_DE_NFFT_SIZE; ++i) buf[i] = i < 150 ? stock[i] : 0.0f;
        result[0] = good >= 15U ? cs_de_ifft(buf) : NAN;
        const float valid_rtt = isfinite(rtt) && rtt > 0.0f ? rtt : NAN;
        result[1] = good >= 15U && isfinite(raw_ifft) ? raw_ifft :
            good >= 15U && isfinite(result[2]) ? result[2] : valid_rtt;
        if (isfinite(raw_ifft) && raw_ifft >= 0.0f) {
            for (size_t i = 0; i < 2 * CONFIG_BT_CS_DE_NFFT_SIZE; ++i) buf[i] = i < 150 ? src[i] : 0.0f;
            if (nrf_cs_iq_gap_fill(buf, counts, 75U) > 0U) {
                float filled = cs_de_ifft(buf);
                if (isfinite(filled) && filled >= 0.0f) result[1] = filled;
            }
        }
        result[3] = (float)nrf_cs_measurement_confidence(good, 15U, isfinite(raw_ifft) || isfinite(result[2]),
            isfinite(rtt) && rtt > 0.0f, result[1], rtt);
        if (fwrite(result, sizeof(result), 1, out) != 1U) return 5;
        if (extra != NULL) {
            /* The production order: the (filled) combined IQ goes through cs_de_ifft once more, which leaves the magnitude profile. */
            float sharp = NAN, tail[4];
            float path_quality[5] = {NAN, NAN, NAN, NAN, NAN};
            if (isfinite(raw_ifft) && raw_ifft >= 0.0f) {
                for (size_t i = 0; i < 2 * CONFIG_BT_CS_DE_NFFT_SIZE; ++i) buf[i] = i < 150 ? src[i] : 0.0f;
                (void)nrf_cs_iq_gap_fill(buf, counts, 75U);
                (void)cs_de_ifft(buf);
                const uint32_t selected = find_ifft_peak_index(buf);
                float maximum = 0.0f;
                for (size_t k = 0; k < CONFIG_BT_CS_DE_NFFT_SIZE; ++k)
                    if (buf[k] > maximum) maximum = buf[k];
                path_quality[0] = buf[selected];
                path_quality[1] = maximum;
                path_quality[2] = (float)selected;
                sharp = nrf_cs_ifft_peak_to_floor(buf, CONFIG_BT_CS_DE_NFFT_SIZE);
                if (maximum > 0.0f) path_quality[3] = sharp * path_quality[0] / maximum;
            }
            const bool pbr = isfinite(raw_ifft) || isfinite(result[2]);
            const bool rtt_ok = isfinite(rtt) && rtt > 0.0f;
            tail[0] = sharp;
            tail[1] = (float)nrf_cs_measurement_confidence(good, 15U, pbr, rtt_ok, result[1], rtt);
            tail[2] = (float)nrf_cs_measurement_confidence_with_peak(good, 15U, pbr, rtt_ok, result[1], rtt, sharp);
            tail[3] = (float)nrf_cs_measurement_confidence_reason(good, 15U, pbr, rtt_ok, result[1], rtt, sharp);
            path_quality[4] = (float)nrf_cs_measurement_confidence_with_peak(
                good, 15U, pbr, rtt_ok, result[1], rtt, path_quality[3]);
            if (fwrite(tail, sizeof(tail), 1, extra) != 1U) return 5;
            if (peak != NULL && fwrite(path_quality, sizeof(path_quality), 1, peak) != 1U) return 5;
        }
        ++procedures;
    }
    fclose(in); fclose(out);
    if (extra != NULL) fclose(extra);
    if (peak != NULL) fclose(peak);
    printf("replayed=%zu\n", procedures);
    return 0;
}
