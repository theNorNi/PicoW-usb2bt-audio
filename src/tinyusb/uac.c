/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jerzy Kasenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

 #include <stdio.h>
 #include <string.h>
 
 #include "bsp/board_api.h"
 #include "tusb.h"
 #include "usb_descriptors.h"

 #include "../btstack/bt_audio.h"
 #include "pico/flash.h"


 
 //--------------------------------------------------------------------+
 // MACRO CONSTANT TYPEDEF PROTOTYPES
 //--------------------------------------------------------------------+
 
 // List of supported sample rates
 const uint32_t sample_rates[] = {44100};
 
 uint32_t current_sample_rate  = 44100;

 uint16_t buffer_counter = 0;
 uint16_t audio_buffer_pool[AUDIO_BUF_POOL_LEN] = {0};
 bool need_change_bt_volume = false;
 
 #define N_SAMPLE_RATES  TU_ARRAY_SIZE(sample_rates)
 
 /* Blink pattern
  * - 25 ms   : streaming data
  * - 250 ms  : device not mounted
  * - 1000 ms : device mounted
  * - 2500 ms : device is suspended
  */
 enum
 {
   BLINK_STREAMING = 25,
   BLINK_NOT_MOUNTED = 250,
   BLINK_MOUNTED = 1000,
   BLINK_SUSPENDED = 2500,
 };
 
 enum
 {
   VOLUME_CTRL_0_DB = 0,
   VOLUME_CTRL_10_DB = 2560,
   VOLUME_CTRL_20_DB = 5120,
   VOLUME_CTRL_30_DB = 7680,
   VOLUME_CTRL_40_DB = 10240,
   VOLUME_CTRL_50_DB = 12800,
   VOLUME_CTRL_60_DB = 15360,
   VOLUME_CTRL_70_DB = 17920,
   VOLUME_CTRL_80_DB = 20480,
   VOLUME_CTRL_90_DB = 23040,
   VOLUME_CTRL_100_DB = 25600,
   VOLUME_CTRL_SILENCE = 0x8000,
 };

 #define BT_VOL_MAX   127
 #define USB_ATT_MAX  25600
 
 static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
 
 // Audio controls
 // Current states
 int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];       // +1 for master channel 0
 int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0
 
 int16_t volume0_last = 0;
 int8_t mute0_last = 0; 

 // Buffer for microphone data
 //int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];

 // Buffer for speaker data
 int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
 // Speaker data size received in the last frame
 int spk_data_size;
 // Resolution per format
 const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                         CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};
 // Current resolution, update on format change
 uint8_t current_resolution;
 
 //void led_blinking_task(void);
 void audio_task(void);
 void audio_control_task(void);
 
 /*------------- MAIN -------------*/
 void tinyusb_main(void)
 {


  flash_safe_execute_core_init();

  //board_init();
 
   // init device stack on configured roothub port
   tusb_rhport_init_t dev_init = {
     .role = TUSB_ROLE_DEVICE,
     .speed = TUSB_SPEED_AUTO
   };
   tusb_init(BOARD_TUD_RHPORT, &dev_init);


  // share the audio pool buf to BT 
  set_shared_audio_buffer(audio_buffer_pool);

 }


 void tinyusb_task(void){
    tud_task(); // TinyUSB device task
    audio_task(); 
 }
 

 void tinyusb_control_task(void){
  audio_control_task();
  }

 //--------------------------------------------------------------------+
 // Device callbacks
 //--------------------------------------------------------------------+
 
 // Invoked when device is mounted
 void tud_mount_cb(void)
 {
   //blink_interval_ms = BLINK_MOUNTED;
 }
 
 // Invoked when device is unmounted
 void tud_umount_cb(void)
 {
   //blink_interval_ms = BLINK_NOT_MOUNTED;
 }
 
 // Invoked when usb bus is suspended
 // remote_wakeup_en : if host allow us  to perform remote wakeup
 // Within 7ms, device must draw an average of current less than 2.5 mA from bus
 void tud_suspend_cb(bool remote_wakeup_en)
 {
   (void)remote_wakeup_en;
   printf("tud_suspend_cb\n");
 }
 
 // Invoked when usb bus is resumed
 void tud_resume_cb(void)
 {
  printf("tud_resume_cb\n");
 }
 
 // Helper for clock get requests
 static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
 
   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       TU_LOG1("Clock get current freq %" PRIu32 "\r\n", current_sample_rate);
 
       audio_control_cur_4_t curf = { (int32_t) tu_htole32(current_sample_rate) };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
     }
     else if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_4_n_t(N_SAMPLE_RATES) rangef =
       {
         .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
       };
       TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
       for(uint8_t i = 0; i < N_SAMPLE_RATES; i++)
       {
         rangef.subrange[i].bMin = (int32_t) sample_rates[i];
         rangef.subrange[i].bMax = (int32_t) sample_rates[i];
         rangef.subrange[i].bRes = 0;
         TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int)rangef.subrange[i].bMin, (int)rangef.subrange[i].bMax, (int)rangef.subrange[i].bRes);
       }
 
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
     }
   }
   else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
            request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t cur_valid = { .bCur = 1 };
     TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
   }
   TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
   return false;
 }
 
 // Helper for clock set requests
 static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
 {
   (void)rhport;
 
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
 
   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
 
     current_sample_rate = (uint32_t) ((audio_control_cur_4_t const *)buf)->bCur;
 
     TU_LOG1("Clock set current freq: %" PRIu32 "\r\n", current_sample_rate);
 
     return true;
   }
   else
   {
     TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
     return false;
   }
 }
 
 // Helper for feature unit get requests
 static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
 
   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t mute1 = { .bCur = mute[request->bChannelNumber] };
     TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_2_n_t(1) range_vol = {
         .wNumSubRanges = tu_htole16(1),
         .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
       };
       TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber,
               range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
     }
     else if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(volume[request->bChannelNumber]) };
       TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
     }
   }
   TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
 
   return false;
 }
 
 // Helper for feature unit set requests
 static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
 {
   (void)rhport;
 
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
 
   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
 
     mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;

     if(request->bChannelNumber == 0){
      need_change_bt_volume = true;
     }
 
     TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, mute[request->bChannelNumber]);
 
     return true;
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
 
     volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
 
     TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, volume[request->bChannelNumber]/256);

     if(request->bChannelNumber == 0){
      need_change_bt_volume = true;
     }
 
     return true;
   }
   else
   {
     TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
     return false;
   }
 }
 
 //--------------------------------------------------------------------+
 // Application Callback API Implementations
 //--------------------------------------------------------------------+
 
 // Invoked when audio class specific get request received for an entity
 bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
 {
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;
 
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_get_request(rhport, request);
   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
     return tud_audio_feature_unit_get_request(rhport, request);
   else
   {
     TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
   }
   return false;
 }
 
 // Invoked when audio class specific set request received for an entity
 bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
 {
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;
 
   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
     return tud_audio_feature_unit_set_request(rhport, request, buf);
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_set_request(rhport, request, buf);
   TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
 
   return false;
 }
 
 bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const * p_request)
 {
   (void)rhport;
 
   uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
   uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
 
   if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt == 0)
       blink_interval_ms = BLINK_MOUNTED;
 
   return true;
 }
 
 bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request)
 {
   (void)rhport;
   uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
   uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
 
   TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
   if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0)
       blink_interval_ms = BLINK_STREAMING;
 
   // Clear buffer when streaming format is changed
   spk_data_size = 0;
   if(alt != 0)
   {
     current_resolution = resolutions_per_format[alt-1];
   }
 
   return true;
 }
 
 bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
 {
   (void)rhport;
   (void)func_id;
   (void)ep_out;
   (void)cur_alt_setting;
 
   spk_data_size = tud_audio_read(spk_buf, n_bytes_received);
   return true;
 }
 
//  bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
//  {
//    (void)rhport;
//    (void)itf;
//    (void)ep_in;
//    (void)cur_alt_setting;
 
//    // This callback could be used to fill microphone data separately
//    return true;
//  }
 
 //--------------------------------------------------------------------+
 // AUDIO Task
 //--------------------------------------------------------------------+

uint16_t usb_stop_delay = 0;

 void audio_task(void)
 {
   if (spk_data_size)
   {
    usb_stop_delay = 0;
    set_usb_streaming(true);
    //printf("currect data size is %d\n", spk_data_size);
    if (current_resolution == 16)
    {
      int16_t *src = (int16_t *)spk_buf;
      uint16_t sample_count = spk_data_size / 4; // should be 44-45

      // Check if usb_audio_buf_counter is in the range of shared_audio_counter and shared_audio_counter + num_audio_samples_per_sbc_buffer * 2
      // 128 is a not good value; need get from btstack_sbc_encoder_sbc_buffer_length()
      if (get_bt_buf_counter() < buffer_counter && buffer_counter < (get_bt_buf_counter() + 128 * 2)){
      // If so, wait until more data is written
          buffer_counter += sample_count;
        }

        for (int i = 0; i < sample_count * 2; i++)
        {
            int16_t sample = src[i];

            if (buffer_counter >= AUDIO_BUF_POOL_LEN)
                buffer_counter = 0;

            audio_buffer_pool[buffer_counter++] = (int16_t)sample;
        }

      set_usb_buf_counter(buffer_counter);
      spk_data_size = 0;
    }
   } else{
    usb_stop_delay++;
    if (usb_stop_delay > 1000){
      set_usb_streaming(false);
    }
   }
 }
 

void audio_control_task(void)
 {
   if (*get_is_bt_sink_volume_changed_ptr())
   {

    uint8_t bt_level = get_bt_volume();

    mute[0] = get_bt_mute();

    // 2) invert & scale into 0…25600
    //    (BT_VOL_MAX - bt_level) maps 127→0, 0→127
    //    multiply then divide with rounding
    uint16_t usb_level = (bt_level > BT_VOL_MAX)
                       ? USB_ATT_MAX
                       : (uint16_t)(( (uint32_t)(BT_VOL_MAX - bt_level)
                                     * USB_ATT_MAX
                                     + (BT_VOL_MAX/2) )
                                   / BT_VOL_MAX);

    // 3) store into USB-Audio’s volume (attenuation) field
    volume[0] = -1 * usb_level / 2;

     // 6.1 Interrupt Data Message
     const audio_interrupt_data_t data = {
       .bInfo = 0,                                       // Class-specific interrupt, originated from an interface
       .bAttribute = AUDIO_CS_REQ_CUR,                   // Caused by current settings
       .wValue_cn_or_mcn = 0,                            // CH0: master volume
       .wValue_cs = AUDIO_FU_CTRL_VOLUME,                // Volume change
       .wIndex_ep_or_int = 0,                            // From the interface itself
       .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT, // From feature unit
     };
 
     tud_audio_int_write(&data);
     *get_is_bt_sink_volume_changed_ptr() = false;
   }

   if (need_change_bt_volume){

    //printf("vol is %d\n", volume[0]);
    
    if(mute[0] == 1 && volume0_last == volume[0]){
      if (mute0_last == 1){
        set_bt_volume(volume[0]/256);
        mute0_last = 0;
        mute[0] = 0;
      }else{
        mute0_last = 1;
        set_bt_volume(-50);
      }
    }else{
      set_bt_volume(volume[0]/256);
    }
    volume0_last = volume[0];
    need_change_bt_volume = false;

    const audio_interrupt_data_t data = {
      .bInfo = 0,                                       // Class-specific interrupt, originated from an interface
      .bAttribute = AUDIO_CS_REQ_CUR,                   // Caused by current settings
      .wValue_cn_or_mcn = 0,                            // CH0: master volume
      .wValue_cs = AUDIO_FU_CTRL_VOLUME,                // Volume change
      .wIndex_ep_or_int = 0,                            // From the interface itself
      .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT, // From feature unit
    };

    tud_audio_int_write(&data);
    //*get_is_bt_sink_volume_changed_ptr() = false;
   }

  }
 