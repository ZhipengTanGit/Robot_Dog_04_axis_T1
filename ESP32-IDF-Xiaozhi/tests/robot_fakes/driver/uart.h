#pragma once
#include <cstddef>
using uart_port_t=int;
constexpr int UART_NUM_1=1,UART_DATA_8_BITS=8,UART_PARITY_DISABLE=0,UART_STOP_BITS_1=1,UART_HW_FLOWCTRL_DISABLE=0,UART_SCLK_DEFAULT=0,UART_PIN_NO_CHANGE=-1;
struct uart_config_t {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;};
int uart_param_config(int,const uart_config_t*);
int uart_set_pin(int,int,int,int,int);
int uart_driver_install(int,int,int,int,void*,int);
int uart_driver_delete(int);
int uart_write_bytes(int,const void*,size_t);
int uart_read_bytes(int,void*,unsigned,unsigned);
