#include "captive_dns_packet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwip/inet.h"


#define DNS_TYPE_A              1
#define DNS_CLASS_IN            1

#define DNS_FLAG_RESPONSE       0x8000
#define DNS_FLAG_AUTHORITATIVE  0x0400
#define DNS_FLAG_RECURSION      0x0100

#define DNS_POINTER_QUESTION    0xC00C

#define DNS_TTL_SECONDS         60


typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;

} dns_header_t;


typedef struct __attribute__((packed))
{
    uint16_t name_pointer;
    uint16_t type;
    uint16_t class_value;
    uint32_t ttl;
    uint16_t address_length;
    uint32_t address;

} dns_a_answer_t;


static uint16_t captive_dns_read_u16(
    const uint8_t *data
)
{
    uint16_t value;

    memcpy(
        &value,
        data,
        sizeof(value)
    );

    return ntohs(value);
}


/*
 * Locate the end of the first DNS question.
 *
 * DNS requests normally use an uncompressed name in
 * the question section.
 */
static bool captive_dns_find_question_end(
    const uint8_t *request,
    size_t request_length,
    size_t *question_end
)
{
    size_t position =
        sizeof(dns_header_t);


    while (position < request_length) {

        const uint8_t label_length =
            request[position];


        if (label_length == 0) {

            position++;

            break;
        }


        /*
         * Compressed names are not expected in the question
         * section of the client queries handled here.
         */
        if ((label_length & 0xC0) != 0) {
            return false;
        }


        if (label_length > 63) {
            return false;
        }


        if (
            position +
            1 +
            label_length >
            request_length
        ) {
            return false;
        }


        position +=
            1 +
            label_length;
    }


    /*
     * QTYPE + QCLASS.
     */
    if (
        position + 4 >
        request_length
    ) {
        return false;
    }


    *question_end =
        position + 4;


    return true;
}


int captive_dns_packet_build_response(
    const uint8_t *request,
    size_t request_length,
    uint32_t ap_ip_addr,
    uint8_t *response,
    size_t response_capacity
)
{
    if (
        request == NULL ||
        response == NULL ||
        request_length <
            sizeof(dns_header_t)
    ) {
        return -1;
    }


    const dns_header_t *request_header =
        (const dns_header_t *)request;


    const uint16_t question_count =
        ntohs(
            request_header->question_count
        );


    /*
     * Captive portal DNS traffic normally contains
     * exactly one question.
     */
    if (question_count != 1) {
        return -1;
    }


    size_t question_end = 0;


    if (
        !captive_dns_find_question_end(
            request,
            request_length,
            &question_end
        )
    ) {
        return -1;
    }


    const uint16_t question_type =
        captive_dns_read_u16(
            request +
            question_end -
            4
        );


    const uint16_t question_class =
        captive_dns_read_u16(
            request +
            question_end -
            2
        );


    const bool redirect_to_ap =
        question_type == DNS_TYPE_A &&
        question_class == DNS_CLASS_IN;


    const size_t answer_size =
        redirect_to_ap
            ? sizeof(dns_a_answer_t)
            : 0;


    if (
        question_end +
        answer_size >
        response_capacity
    ) {
        return -1;
    }


    /*
     * Copy only the header and first question.
     *
     * Additional EDNS records from the request are intentionally
     * not copied into the response.
     */
    memcpy(
        response,
        request,
        question_end
    );


    dns_header_t *response_header =
        (dns_header_t *)response;


    const uint16_t request_flags =
        ntohs(
            request_header->flags
        );


    uint16_t response_flags =
        DNS_FLAG_RESPONSE |
        DNS_FLAG_AUTHORITATIVE;


    /*
     * Preserve the client's recursion-desired flag.
     */
    response_flags |=
        request_flags &
        DNS_FLAG_RECURSION;


    response_header->flags =
        htons(
            response_flags
        );


    response_header->question_count =
        htons(1);


    response_header->answer_count =
        htons(
            redirect_to_ap
                ? 1
                : 0
        );


    response_header->authority_count =
        0;


    response_header->additional_count =
        0;


    if (!redirect_to_ap) {

        /*
         * For AAAA and other unsupported queries return
         * a valid DNS response without an answer.
         */
        return (int)question_end;
    }


    dns_a_answer_t *answer =
        (dns_a_answer_t *)(
            response +
            question_end
        );


    answer->name_pointer =
        htons(
            DNS_POINTER_QUESTION
        );


    answer->type =
        htons(
            DNS_TYPE_A
        );


    answer->class_value =
        htons(
            DNS_CLASS_IN
        );


    answer->ttl =
        htonl(
            DNS_TTL_SECONDS
        );


    answer->address_length =
        htons(
            sizeof(uint32_t)
        );


    /*
     * esp_netif stores the IPv4 address in network byte order,
     * so it can be copied directly into the DNS response.
     */
    answer->address =
        ap_ip_addr;


    return (int)(
        question_end +
        sizeof(dns_a_answer_t)
    );
}