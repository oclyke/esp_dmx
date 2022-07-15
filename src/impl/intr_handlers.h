#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_dmx.h"
#include "esp_system.h"
#include "impl/dmx_hal.h"
#include "impl/driver.h"


/**
 * @brief Helper function that takes an RDM UID from a most-significant-byte
 * first buffer and copies it to least-significant-byte first endianness, which
 * is what ESP32 uses.
 * 
 * @note This function looks horrible, but it is the most efficient way to swap
 * endianness of a 6-byte number on the Xtensa compiler, which is important
 * because it will be used exclusively in an interrupt handler.
 *
 * @param buf A pointer to an RDM buffer.
 * @return uint64_t The properly formatted RDM UID.
 */
FORCE_INLINE_ATTR uint64_t uidcpy(const void *buf) {
  uint64_t val;
  ((uint8_t *)&val)[5] = ((uint8_t *)buf)[0];
  ((uint8_t *)&val)[4] = ((uint8_t *)buf)[1];
  ((uint8_t *)&val)[3] = ((uint8_t *)buf)[2];
  ((uint8_t *)&val)[2] = ((uint8_t *)buf)[3];
  ((uint8_t *)&val)[1] = ((uint8_t *)buf)[4];
  ((uint8_t *)&val)[0] = ((uint8_t *)buf)[5];  
  return val;
}

enum {
  DMX_INTR_RX_FIFO_OVERFLOW = UART_INTR_RXFIFO_OVF,  // Interrupt mask that triggers when the UART overflows.
  DMX_INTR_RX_DATA = (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT),  // Interrupt mask that is triggered when it is time to service the receive FIFO.
  DMX_INTR_RX_BREAK = UART_INTR_BRK_DET,  // Interrupt mask that trigger when a DMX break is received.
  DMX_INTR_RX_FRAMING_ERR = (UART_INTR_PARITY_ERR | UART_INTR_RS485_PARITY_ERR | UART_INTR_FRAM_ERR | UART_INTR_RS485_FRM_ERR),  // Interrupt mask that represents a byte framing error.
  DMX_INTR_RX_CLASH = UART_INTR_RS485_CLASH,  // Interrupt mask that represents a DMX collision.
  DMX_INTR_RX_ALL = (DMX_INTR_RX_DATA | DMX_INTR_RX_BREAK | DMX_INTR_RX_FIFO_OVERFLOW | DMX_INTR_RX_FRAMING_ERR | DMX_INTR_RX_CLASH),  // Interrupt mask that represents all receive conditions.

  DMX_INTR_TX_DATA = UART_INTR_TXFIFO_EMPTY,  // Interrupt mask that is triggered when the UART is ready to send data.
  DMX_INTR_TX_DONE = UART_INTR_TX_DONE,  // Interrupt mask that is triggered when the UART has finished writing data.
  DMX_INTR_TX_ALL = (DMX_INTR_TX_DATA | DMX_INTR_TX_DONE),  // Interrupt mask that represents all send conditions.

  DMX_ALL_INTR_MASK = -1  // Interrupt mask for all interrupts.
};

static void IRAM_ATTR dmx_intr_handler(void *arg) {
  const int64_t now = esp_timer_get_time();
  dmx_driver_t *const driver = (dmx_driver_t *)arg;
  dmx_context_t *const hardware = &dmx_context[driver->dmx_num];

  int task_awoken = false;

  while (true) {
    const uint32_t intr_flags = dmx_hal_get_interrupt_status(&hardware->hal);
    if (intr_flags == 0) break;

    // DMX Receive ####################################################
    if (intr_flags & DMX_INTR_RX_FIFO_OVERFLOW) {
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_RX_FIFO_OVERFLOW);

      // Notify task that an error occurred
      if (driver->is_busy && driver->buffer.waiting_task) {
        xTaskNotifyFromISR(driver->buffer.waiting_task, DMX_ERR_DATA_OVERFLOW,
                           eSetValueWithOverwrite, &task_awoken);
      }

      // Indicate driver is finished reading data and reset the FIFO
      driver->is_busy = false;
      dmx_hal_rxfifo_rst(&hardware->hal);
    }
    else if (intr_flags & DMX_INTR_RX_FRAMING_ERR) {
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_RX_FRAMING_ERR);

      // Notify task that an error occurred
      if (driver->is_busy && driver->buffer.waiting_task) {
        xTaskNotifyFromISR(driver->buffer.waiting_task, DMX_ERR_IMPROPER_SLOT,
                           eSetValueWithOverwrite, &task_awoken);
      }

      // Indicate driver is finished reading data and reset the FIFO
      driver->is_busy = false;
      dmx_hal_rxfifo_rst(&hardware->hal);
    }
    else if (intr_flags & DMX_INTR_RX_BREAK) {
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_RX_BREAK);

      // Notify sniffer that the driver is in a DMX break
      driver->is_in_break = true;

      // Send a task notification if it hasn't been sent yet
      if (driver->is_busy) {
        xTaskNotifyFromISR(driver->buffer.waiting_task, DMX_OK,
                           eSetValueWithOverwrite, &task_awoken);
        driver->buffer.size = driver->buffer.head;  // Update packet size guess
      }

      // Indicate a packet is being read, reset head, and reset the FIFO
      driver->is_busy = true;
      driver->buffer.head = 0;
      dmx_hal_rxfifo_rst(&hardware->hal);

      // TODO: reset sniffer values
    }

    else if (intr_flags & DMX_INTR_RX_DATA) {
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_RX_DATA);

      // Driver is not in a DMX break
      driver->is_in_break = false;

      // Determine the timestamp of the last slot
      if (intr_flags & UART_INTR_RXFIFO_TOUT) {
        uint8_t timeout = dmx_hal_get_rx_timeout_threshold(&hardware->hal);
        driver->buffer.last_received_ts = now - (timeout * 44);
      } else {
        driver->buffer.last_received_ts = now;
      }

      // Read from the FIFO if there is data and if the driver is ready
      size_t read_len = DMX_MAX_PACKET_SIZE - driver->buffer.head;
      if (driver->is_busy && read_len > 0) {
        uint8_t *data_ptr = &driver->buffer.data[driver->buffer.head];
        dmx_hal_read_rxfifo(&hardware->hal, data_ptr, &read_len);
        driver->buffer.head += read_len;
      } else {
        dmx_hal_rxfifo_rst(&hardware->hal);
      }

      // Don't process data if driver already has or no task waiting
      if (!driver->is_busy || driver->buffer.waiting_task == NULL) {
        continue;
      }

      // Determine if a full packet has been received
      const uint8_t sc = driver->buffer.data[0];  // Received DMX start code.
      if (sc == DMX_SC) {
        if (driver->buffer.head > driver->buffer.size) {
          xTaskNotifyFromISR(driver->buffer.waiting_task, 0,  // FIXME: use enum
                             eSetValueWithOverwrite, &task_awoken);
          driver->is_busy = false;
        }
      } // TODO: process RDM or RDM Discovery Response
    }

    else if (intr_flags & DMX_INTR_RX_CLASH) {
      // Multiple devices sent data at once (typical of RDM discovery)
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_RX_CLASH);
      // TODO: this code should only run when using RDM
    }

    // DMX Transmit #####################################################
    else if (intr_flags & DMX_INTR_TX_DATA) {
      // UART is ready to write more DMX data
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_TX_DATA);

      // Write data to the UART
      size_t write_size = driver->buffer.size - driver->buffer.head;
      dmx_hal_write_txfifo(&hardware->hal, driver->buffer.data, &write_size);
      driver->buffer.head += write_size;
      
      // Allow FIFO to empty when done writing data
      if (driver->buffer.head == driver->buffer.size) {
        portENTER_CRITICAL_ISR(&hardware->spinlock);
        dmx_hal_disable_interrupt(&hardware->hal, DMX_INTR_TX_DATA);
        portEXIT_CRITICAL_ISR(&hardware->spinlock);
      }
    }
    
    else if (intr_flags & DMX_INTR_TX_DONE) {
      // UART has finished sending DMX data
      dmx_hal_clear_interrupt(&hardware->hal, DMX_INTR_TX_DONE);

      // Record timestamp of last sent slot
      driver->buffer.last_sent_ts = now;
      
      // Set flags and signal data is sent
      driver->is_busy = false;
      xSemaphoreGiveFromISR(driver->data_written, &task_awoken);
    }

    else {
      // disable interrupts that shouldn't be handled
      // this code shouldn't be called but it can prevent crashes when it is
      portENTER_CRITICAL_ISR(&hardware->spinlock);
      dmx_hal_disable_interrupt(&hardware->hal, intr_flags);
      portEXIT_CRITICAL_ISR(&hardware->spinlock);
      dmx_hal_clear_interrupt(&hardware->hal, intr_flags);
    }
  }

  if (task_awoken) portYIELD_FROM_ISR();
}

static void IRAM_ATTR dmx_timing_intr_handler(void *arg) {
  const int64_t now = esp_timer_get_time();
  dmx_driver_t *const driver = (dmx_driver_t *)arg;

  /* If this ISR is called on a positive edge and the current DMX frame is in a
  break and a negative edge condition has already occurred, then the break has
  just finished, so we can update the length of the break as well as unset the
  rx_is_in_brk flag. If this ISR is called on a negative edge and the
  mark-after-break has not been recorded while the break has been recorded,
  then we know that the mark-after-break has just completed so we should record
  its duration. */
/*
  if (dmx_hal_get_rx_level(&(dmx_context[driver->dmx_num].hal))) {
    if (driver->rx.is_in_brk && driver->rx.last_neg_edge_ts > -1) {
      driver->rx.break_len = now - driver->rx.last_neg_edge_ts;
      driver->rx.is_in_brk = false;
    }
    driver->rx.last_pos_edge_ts = now;
  } else {
    if (driver->rx.mab_len == -1 && driver->rx.break_len != -1)
      driver->rx.mab_len = now - driver->rx.last_pos_edge_ts;
    driver->rx.last_neg_edge_ts = now;
  }
  */
}

static bool IRAM_ATTR dmx_timer_intr_handler(void *arg) {
  dmx_driver_t *const driver = (dmx_driver_t *)arg;
  dmx_context_t *const hardware = &dmx_context[driver->dmx_num];
  int task_awoken = false;

  if (driver->is_in_break) {
    // End the DMX break
    dmx_hal_invert_signal(&hardware->hal, 0);
    driver->is_in_break = false;

    // Get the configured length of the DMX mark-after-break
    portENTER_CRITICAL_ISR(&hardware->spinlock);
    const uint32_t mab_len = driver->tx.mab_len;
    portEXIT_CRITICAL_ISR(&hardware->spinlock);

    // Reset the alarm for the end of the DMX mark-after-break
    timer_group_set_alarm_value_in_isr(driver->rst_seq_hw, driver->timer_idx,
                                       mab_len);
  } else {
    // Write data to the UART
    size_t write_size = driver->buffer.size - driver->buffer.head;
    dmx_hal_write_txfifo(&hardware->hal, driver->buffer.data, &write_size);
    driver->buffer.head += write_size;

    // Enable DMX write interrupts
    portENTER_CRITICAL_ISR(&hardware->spinlock);
    dmx_hal_enable_interrupt(&hardware->hal, DMX_INTR_TX_ALL);
    portEXIT_CRITICAL_ISR(&hardware->spinlock);

    // Pause the timer
    timer_pause(driver->rst_seq_hw, driver->timer_idx);
  }

  return task_awoken;
}

#ifdef __cplusplus
}
#endif
