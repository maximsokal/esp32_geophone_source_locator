#ifndef AD7606_DRIVER_HOST_FREERTOS_H
#define AD7606_DRIVER_HOST_FREERTOS_H

typedef unsigned int portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0U

#if defined(AD7606_DRIVER_HOST_TEST)
void ad7606_test_port_enter_critical(portMUX_TYPE *mux);
void ad7606_test_port_exit_critical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) ad7606_test_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux) ad7606_test_port_exit_critical(mux)
#else
static inline void ad7606_host_port_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
}

static inline void ad7606_host_port_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
}

#define portENTER_CRITICAL(mux) ad7606_host_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux) ad7606_host_port_exit_critical(mux)
#endif

#endif
