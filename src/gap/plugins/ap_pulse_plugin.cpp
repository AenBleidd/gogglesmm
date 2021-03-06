/*******************************************************************************
*                         Goggles Audio Player Library                         *
********************************************************************************
*           Copyright (C) 2010-2014 by Sander Jansen. All Rights Reserved      *
*                               ---                                            *
* This program is free software: you can redistribute it and/or modify         *
* it under the terms of the GNU General Public License as published by         *
* the Free Software Foundation, either version 3 of the License, or            *
* (at your option) any later version.                                          *
*                                                                              *
* This program is distributed in the hope that it will be useful,              *
* but WITHOUT ANY WARRANTY; without even the implied warranty of               *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                *
* GNU General Public License for more details.                                 *
*                                                                              *
* You should have received a copy of the GNU General Public License            *
* along with this program.  If not, see http://www.gnu.org/licenses.           *
********************************************************************************/
#include "ap_defs.h"
#include "ap_config.h"
#include "ap_event.h"
#include "ap_pipe.h"
#include "ap_reactor.h"
#include "ap_event_queue.h"
#include "ap_thread_queue.h"
#include "ap_format.h"
#include "ap_device.h"
#include "ap_buffer.h"
#include "ap_packet.h"
#include "ap_engine.h"
#include "ap_thread.h"
#include "ap_input_plugin.h"
#include "ap_output_plugin.h"
#include "ap_decoder_plugin.h"
#include "ap_decoder_thread.h"
#include "ap_output_thread.h"

#include "ap_pulse_plugin.h"
#include <poll.h>


using namespace ap;



class pa_io_event : public Reactor::Input {
public:
  static pa_io_event*      recycle;
  pa_io_event_cb_t         callback;
  pa_io_event_destroy_cb_t destroy_callback;
  void *                   userdata;   
protected:

  /// Convert mode flags to pulseaudio flags
  static pa_io_event_flags_t toPulse(FXuchar mode) {
    int event=PA_IO_EVENT_NULL;
    event=((mode&Reactor::Input::IsReadable)  ? PA_IO_EVENT_INPUT : 0) |
          ((mode&Reactor::Input::IsWritable)  ? PA_IO_EVENT_OUTPUT : 0) |
          ((mode&Reactor::Input::IsException) ? (PA_IO_EVENT_ERROR|PA_IO_EVENT_HANGUP) : 0);
    return (pa_io_event_flags_t)event;
    }

  /// Convert pulseaudio flags to mode flags
  static FXuchar fromPulse(pa_io_event_flags_t flags) {
    return (((flags&PA_IO_EVENT_INPUT)  ? Reactor::Input::Readable  : 0) | 
            ((flags&PA_IO_EVENT_OUTPUT) ? Reactor::Input::Writable  : 0) |
            ((flags&PA_IO_EVENT_ERROR)  ? Reactor::Input::Exception : 0) |
            ((flags&PA_IO_EVENT_HANGUP) ? Reactor::Input::Exception : 0));
    }
public:
  pa_io_event(FXInputHandle h,FXuchar m) : Reactor::Input(h,m) {
    }

  virtual void onSignal() {
    callback(&(PulseOutput::instance->api),this,handle,toPulse(mode),userdata);
    }

  static pa_io_event * create(pa_mainloop_api*,int fd,pa_io_event_flags_t events, pa_io_event_cb_t cb, void *userdata) {
    pa_io_event * event;
    if (recycle) {
      event         = recycle;
      event->handle = fd;
      event->mode   = fromPulse(events);
      recycle       = NULL;
      }
    else {
      event = new pa_io_event(fd,fromPulse(events));
      }
    event->callback         = cb;
    event->userdata         = userdata;
    event->destroy_callback = NULL;
    PulseOutput::instance->output->getReactor().addInput(event);
    return event;
    }

  static void destroy(pa_io_event* event){
    if (event->destroy_callback)
      event->destroy_callback(&PulseOutput::instance->api,event,event->userdata);
    PulseOutput::instance->output->getReactor().removeInput(event);
    if (recycle==NULL) 
      recycle=event;
    else
      delete event;
    }

  static void enable(pa_io_event*event,pa_io_event_flags_t events) {
    event->mode = fromPulse(events);
    }

  static void set_destroy(pa_io_event *event, pa_io_event_destroy_cb_t cb){
    event->destroy_callback=cb;
    }
  };





class pa_time_event : public Reactor::Timer {
public:
  static pa_time_event*       recycle;
  pa_time_event_cb_t          callback;
  pa_time_event_destroy_cb_t  destroy_callback;
  void*                       userdata;
public:
  pa_time_event() : callback(NULL),destroy_callback(NULL),userdata(NULL) {}

  virtual void onExpired() {
    struct timeval tv;
    tv.tv_usec=(time/1000)%1000000;
    tv.tv_sec=time/1000000000;
    callback(&(PulseOutput::instance->api),this,&tv,userdata);
    }

  static pa_time_event * create(pa_mainloop_api*,const struct timeval *tv, pa_time_event_cb_t cb, void *userdata) {
    FXTime time = (tv->tv_sec*1000000000) + (tv->tv_usec*1000); 
    pa_time_event * event;
    if (recycle) {
      event   = recycle;
      recycle = NULL; 
      }
    else {
      event = new pa_time_event();
      }
    event->callback = cb;
    event->userdata = userdata;
    PulseOutput::instance->output->getReactor().addTimer(event,time);
    return event;
    }

  static void destroy(pa_time_event* event){
    if (event->destroy_callback)
      event->destroy_callback(&PulseOutput::instance->api,event,event->userdata);
    PulseOutput::instance->output->getReactor().removeTimer(event);
    if (recycle==NULL)
      recycle=event;
    else
      delete event;
    }

  static void restart(pa_time_event* event, const struct timeval *tv){
    FXTime time = (tv->tv_sec*1000000000) + (tv->tv_usec*1000); 
    PulseOutput::instance->output->getReactor().removeTimer(event);
    PulseOutput::instance->output->getReactor().addTimer(event,time);
    }    

  static void set_destroy(pa_time_event *event, pa_time_event_destroy_cb_t cb){
    event->destroy_callback=cb;
    }
  };








class pa_defer_event : public Reactor::Deferred {  
public:
  static pa_defer_event*      recycle;
  pa_defer_event_cb_t         callback;
  pa_defer_event_destroy_cb_t destroy_callback;
  void*                       userdata;
public:
  pa_defer_event() {}
    
  virtual void run() {
    callback(&PulseOutput::instance->api,this,userdata);
    }
    
  static pa_defer_event* create(pa_mainloop_api*, pa_defer_event_cb_t cb, void *userdata){
    pa_defer_event * event;
    if (recycle) {
      event   = recycle;
      recycle = NULL;
      }
    else  {
      event = new pa_defer_event();
      } 
    event->callback         = cb;
    event->userdata         = userdata;
    event->destroy_callback = NULL;    

    PulseOutput::instance->output->getReactor().addDeferred(event);
    return event;
    };

  static void toggle_enabled(pa_defer_event*event,int b) {
    if (b)
      event->enable();
    else
      event->disable();
    }

  static void destroy(pa_defer_event* event){
    if (event->destroy_callback)
      event->destroy_callback(&PulseOutput::instance->api,event,event->userdata);

    PulseOutput::instance->output->getReactor().removeDeferred(event);

    if (recycle==NULL)
      recycle = event;
    else
      delete event;
    }

  static void set_destroy(pa_defer_event *event, pa_defer_event_destroy_cb_t cb){
    event->destroy_callback=cb;
    }
  };


pa_io_event    * pa_io_event::recycle;
pa_time_event  * pa_time_event::recycle;
pa_defer_event * pa_defer_event::recycle;

void pulse_quit(pa_mainloop_api*,int){  
  GM_DEBUG_PRINT("pulse_quit\n");
  }


extern "C" GMAPI OutputPlugin * ap_load_plugin(OutputThread * output) {
  return new PulseOutput(output);
  }

extern "C" GMAPI void ap_free_plugin(OutputPlugin* plugin) {
  delete plugin;
  }

namespace ap {


PulseOutput * PulseOutput::instance = NULL;

PulseOutput::PulseOutput(OutputThread * thread) : OutputPlugin(thread),context(NULL),stream(NULL),svolume(PA_VOLUME_MUTED) {
  instance              = this;
  api.userdata          = this;
  api.io_new            = pa_io_event::create; //pulse_io_new;
  api.io_free           = pa_io_event::destroy;// pulse_io_free;
  api.io_enable         = pa_io_event::enable; //pulse_io_enable;
  api.io_set_destroy    = pa_io_event::set_destroy;//pulse_io_set_destroy;
  api.defer_new         = pa_defer_event::create; //pulse_defer_new;
  api.defer_free        = pa_defer_event::destroy; //pulse_defer_free;
  api.defer_enable      = pa_defer_event::toggle_enabled; //pulse_defer_enable;
  api.defer_set_destroy = pa_defer_event::set_destroy; //pulse_defer_set_destroy;
  api.time_new          = pa_time_event::create;  //pulse_time_new;
  api.time_restart      = pa_time_event::restart; //pulse_time_restart;
  api.time_free         = pa_time_event::destroy; //pulse_time_free;
  api.time_set_destroy  = pa_time_event::set_destroy; //pulse_time_set_destroy;
  api.quit              = pulse_quit;

  pa_io_event::recycle    = NULL;
  pa_time_event::recycle  = NULL;
  pa_defer_event::recycle = NULL;
  }

PulseOutput::~PulseOutput() {
  close();
  }

static FXbool to_pulse_format(const AudioFormat & af,pa_sample_format & pulse_format){
  switch(af.format) {
    case AP_FORMAT_S16_LE   : pulse_format=PA_SAMPLE_S16LE; break;
    case AP_FORMAT_S16_BE   : pulse_format=PA_SAMPLE_S16BE; break;
    case AP_FORMAT_S24_LE   : pulse_format=PA_SAMPLE_S24_32LE;  break;
    case AP_FORMAT_S24_BE   : pulse_format=PA_SAMPLE_S24_32BE;  break;
    case AP_FORMAT_S24_3LE  : pulse_format=PA_SAMPLE_S24LE;  break;
    case AP_FORMAT_S24_3BE  : pulse_format=PA_SAMPLE_S24BE;  break;
    case AP_FORMAT_FLOAT_LE : pulse_format=PA_SAMPLE_FLOAT32LE;  break;
    case AP_FORMAT_FLOAT_BE : pulse_format=PA_SAMPLE_FLOAT32BE;  break;
    default                 : return false; break;
    }
  return true;
  }


static FXbool to_gap_format(pa_sample_format pulse_format,AudioFormat & af){
  switch(pulse_format) {
    case PA_SAMPLE_U8       : af.format = AP_FORMAT_U8;       break;
    case PA_SAMPLE_S16LE    : af.format = AP_FORMAT_S16_LE;   break;
    case PA_SAMPLE_S16BE    : af.format = AP_FORMAT_S16_BE;   break;
    case PA_SAMPLE_S24LE    : af.format = AP_FORMAT_S24_3LE;   break;
    case PA_SAMPLE_S24BE    : af.format = AP_FORMAT_S24_3BE;   break;
    case PA_SAMPLE_S24_32LE : af.format = AP_FORMAT_S24_LE;  break;
    case PA_SAMPLE_S24_32BE : af.format = AP_FORMAT_S24_BE;  break;
    case PA_SAMPLE_FLOAT32LE: af.format = AP_FORMAT_FLOAT_LE; break;
    case PA_SAMPLE_FLOAT32BE: af.format = AP_FORMAT_FLOAT_BE; break;
    default                 : return false;
    }
  return true;
  }



#ifdef DEBUG
static void context_state_callback(pa_context *c,void*){
  GM_DEBUG_PRINT("context_state_callback %d ",pa_context_get_state(c));
  switch(pa_context_get_state(c)) {
    case PA_CONTEXT_UNCONNECTED : fxmessage(" unconnected\n"); break;
    case PA_CONTEXT_CONNECTING  : fxmessage(" connecting\n"); break;
    case PA_CONTEXT_AUTHORIZING : fxmessage(" authorizing\n"); break;
    case PA_CONTEXT_SETTING_NAME: fxmessage(" setting name\n"); break;
    case PA_CONTEXT_READY       : fxmessage(" ready\n"); break;
    case PA_CONTEXT_FAILED      : fxmessage(" failed\n"); break;
    case PA_CONTEXT_TERMINATED  : fxmessage(" terminated\n"); break;
    };
  }

static void stream_state_callback(pa_stream *s,void*){
  GM_DEBUG_PRINT("stream_state_callback %d\n",pa_stream_get_state(s));
  }
#endif

//static void stream_write_callback(pa_stream*s,size_t,void *){
//  GM_DEBUG_PRINT("stream_write_callback %d\n",pa_stream_get_state(s));
//  }

void PulseOutput::sink_info_callback(pa_context*, const pa_sink_input_info * info,int /*eol*/,void*userdata){
  PulseOutput * out = reinterpret_cast<PulseOutput*>(userdata);
  if (info) {
    pa_volume_t v = pa_cvolume_avg(&info->volume);
    float vol = (float) v / (float)PA_VOLUME_NORM ;
    if (out->svolume!=v)
      out->output->notify_volume(vol);
    }
  }

void PulseOutput::context_subscribe_callback(pa_context * context, pa_subscription_event_type_t type, uint32_t index, void *userdata){
  PulseOutput * out = reinterpret_cast<PulseOutput*>(userdata);

  if (out->stream==NULL)
    return;

  if (pa_stream_get_index(out->stream)!=index)
    return;

  if ((type&PA_SUBSCRIPTION_EVENT_FACILITY_MASK)!=PA_SUBSCRIPTION_EVENT_SINK_INPUT)
    return;

  if ((type&PA_SUBSCRIPTION_EVENT_TYPE_MASK)==PA_SUBSCRIPTION_EVENT_CHANGE || 
      (type&PA_SUBSCRIPTION_EVENT_TYPE_MASK)==PA_SUBSCRIPTION_EVENT_NEW) {
    pa_operation *operation = pa_context_get_sink_input_info(context,index,sink_info_callback,userdata);
    if (operation) pa_operation_unref(operation);
    }
  }




FXbool PulseOutput::open() {

  /// Start the mainloop
  //if (eventloop==NULL) {
   // eventloop = new PulseReactor();
   // }

  /// Get a context
  if (context==NULL) {
    context = pa_context_new(&api,"Goggles Music Manager");
#ifdef DEBUG
    pa_context_set_state_callback(context,context_state_callback,this);
#endif
    pa_context_set_subscribe_callback(context,context_subscribe_callback,this);
    }

  /// Try connecting
  GM_DEBUG_PRINT("pa_context_connect()\n");
  if (pa_context_get_state(context)==PA_CONTEXT_UNCONNECTED) {
    if (pa_context_connect(context,NULL,PA_CONTEXT_NOFLAGS,NULL)<0) {
      GM_DEBUG_PRINT("pa_context_connect failed\n");
      return false;
      }
    }

  /// Wait until we're connected to the pulse daemon
  GM_DEBUG_PRINT("wait for connection\n");
  pa_context_state_t state;
  while((state=pa_context_get_state(context))!=PA_CONTEXT_READY) {
    if (state==PA_CONTEXT_FAILED || state==PA_CONTEXT_TERMINATED){
      GM_DEBUG_PRINT("Unable to connect to pulsedaemon\n");
      return false;
      }
    output->wait_plugin_events();
    }

  pa_operation* operation = pa_context_subscribe(context,PA_SUBSCRIPTION_MASK_SINK_INPUT,NULL,this);
  if (operation) pa_operation_unref(operation);

  GM_DEBUG_PRINT("ready()\n");
  return true;
  }

void PulseOutput::close() {
#ifdef DEBUG
  output->getReactor().debug();    
#endif

  if (stream) {
    GM_DEBUG_PRINT("disconnecting stream\n");
    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    stream=NULL;
    }

  if (context) {
    GM_DEBUG_PRINT("disconnecting context\n");
    pa_context_disconnect(context);
    pa_context_unref(context);
    context=NULL;
    }
#ifdef DEBUG
  output->getReactor().debug();    
#endif

  delete pa_io_event::recycle;
  delete pa_time_event::recycle;
  delete pa_defer_event::recycle;
  pa_io_event::recycle    = NULL;
  pa_time_event::recycle  = NULL;
  pa_defer_event::recycle = NULL;


/*
  if (mainloop) {
    GM_DEBUG_PRINT("disconnecting mainloop\n");    
    pa_threaded_mainloop_unlock(mainloop);
    pa_threaded_mainloop_stop(mainloop);
    pa_threaded_mainloop_free(mainloop);
    mainloop=NULL;
    }
*/
  af.reset();
  }




void PulseOutput::volume(FXfloat v) {
  if (context && stream) {
    svolume = (pa_volume_t)(v*PA_VOLUME_NORM);
    pa_cvolume cvol;
    pa_cvolume_set(&cvol,af.channels,svolume);
    pa_operation* operation = pa_context_set_sink_input_volume(context,pa_stream_get_index(stream),&cvol,NULL,NULL);
    pa_operation_unref(operation);
    }
  }

FXint PulseOutput::delay() {
  FXint value=0;
  if (stream) {
    pa_usec_t latency;
    int negative;
    if (pa_stream_get_latency(stream,&latency,&negative)>=0){
      value = (latency*af.rate) / 1000000;
      }
    }
  return value;
  }

void PulseOutput::drop() {
  if (stream) {
    pa_operation* operation = pa_stream_flush(stream,NULL,0);
    pa_operation_unref(operation);
    }
  }

void PulseOutput::drain() {
  if (stream) {
    pa_operation * operation = pa_stream_drain(stream,NULL,NULL);
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
      output->wait_plugin_events();
    pa_operation_unref(operation);
    }
  }

void PulseOutput::pause(FXbool) {
  }

FXbool PulseOutput::configure(const AudioFormat & fmt){
  const pa_sample_spec * config=NULL;

  if (!open())
    return false;

  if (stream && fmt==af)
    return true;

  if (stream) {
    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    stream=NULL;
    }

  pa_sample_spec spec;

  if (!to_pulse_format(fmt,spec.format))
    goto failed;

  spec.rate     = fmt.rate;
  spec.channels = fmt.channels;

  stream = pa_stream_new(context,"Goggles Music Manager",&spec,NULL);
#ifdef DEBUG
  pa_stream_set_state_callback(stream,stream_state_callback,this);
#endif
  //pa_stream_set_write_callback(stream,stream_write_callback,this);

  if (pa_stream_connect_playback(stream,NULL,NULL,PA_STREAM_NOFLAGS,NULL,NULL)<0)
    goto failed;

  /// Wait until stream is ready
  pa_stream_state_t state;
  while((state=pa_stream_get_state(stream))!=PA_STREAM_READY) {
    if (state==PA_STREAM_FAILED || state==PA_STREAM_TERMINATED){
      goto failed;
      }
    output->wait_plugin_events();
    } 

  /// Get Actual Format
  config = pa_stream_get_sample_spec(stream);
  if (!to_gap_format(config->format,af))
    goto failed;
  af.channels=config->channels;
  af.rate=config->rate;
  return true;
failed:
  GM_DEBUG_PRINT("Unsupported pulse configuration:\n");
  af.debug();
  return false;
  }

FXbool PulseOutput::write(const void * b,FXuint nframes){
  FXASSERT(stream);
  const FXchar * buffer = reinterpret_cast<const FXchar*>(b);
  FXuint total = nframes*af.framesize();
  while(total) {
    size_t nbytes = pa_stream_writable_size(stream);
    size_t n = FXMIN(total,nbytes);
    if (n<=0) {
      //fxmessage("size %ld\n",nbytes);
      output->wait_plugin_events();
      continue;
      }
    pa_stream_write(stream,buffer,n,NULL,0,PA_SEEK_RELATIVE);
    total-=n;
    buffer+=n;
    }
  return true;
  }




}
