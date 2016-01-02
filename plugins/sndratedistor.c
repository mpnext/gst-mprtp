/* GStreamer Scheduling tree
 * Copyright (C) 2015 Balázs Kreith (contact: balazs.kreith@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "sndratedistor.h"
#include <math.h>
#include <gst/gst.h>
#include <stdlib.h>
#include "percentiletracker.h"
#include <string.h>
#include "streamsplitter.h"
#include "sefctrler.h"
#include "nlms.h"


GST_DEBUG_CATEGORY_STATIC (sndrate_distor_debug_category);
#define GST_CAT_DEFAULT sndrate_distor_debug_category
#define MEASUREMENT_LENGTH 3
#define INCREASEMENT_LENGTH 16
G_DEFINE_TYPE (SendingRateDistributor, sndrate_distor, G_TYPE_OBJECT);


typedef struct _Subflow Subflow;
typedef struct _Measurement Measurement;


typedef enum{
  STATE_OVERUSED       = -1,
  STATE_STABLE         =  0,
  STATE_MONITORED       = 1,
}State;

typedef State (*Checker)(SendingRateDistributor*,Subflow*);

typedef struct _UtilizationReport{
  guint32  actual_target;
  guint32  desired_target;
  guint32  actual_rate;
  guint32  desired_rate;
  gboolean greedy;
  struct{
    gdouble  target_weight;
    gboolean available;
  }subflows[32];
}UtilizationReport;


typedef struct _CorrValue{
  gint32  saved[4];
  guint   index;
  gint32  sum;
  gdouble value;
}CorrValue;

struct _Measurement{
  guint16         PiT;
  gdouble         corrh_owd;
  gdouble         corrd;
  guint64         delay;
  guint32         jitter;
  gdouble         goodput;
  guint32         lost;
  guint32         discard;
  gdouble         discard_rate;
  guint32         receiver_rate;

  gdouble         goodput_ratio;
  gdouble         delay_ratio;

  State           state;
};



struct _Subflow{
  guint8             id;
  MPRTPSPath*        path;
  gboolean           available;

  gint32             requested_bytes;
  gint32             supplied_bytes;
  gint32             movable_bytes;

  guint8             overused_history;

  gdouble            weight;
  gdouble            target_weight;
  gdouble            bounce_point;
  gint32             sending_rate;
  gint32             target_rate;
  gint32             step_rate;

  //Need for monitoring
  guint              monitoring_interval;
  guint              monitoring_time;

  guint              disable_controlling;
  Checker            controlling;
  guint              turning_point;
  guint              monitoring_disabled;

  //need for state transitions
  State              state;
  PercentileTracker *delays;
  PercentileTracker *goodputs;
  NormalizedLeastMeanSquere *falls;
  gint32             falling_point;

  gdouble            bandwidth_estimation;
  gdouble            bandwidth_estimation_error;

  //Need for measurements
  gboolean           measured;
  Measurement        measurements[MEASUREMENT_LENGTH];
  guint8             measurements_index;

};

//----------------------------------------------------------------------
//-------- Private functions belongs to the object ----------
//----------------------------------------------------------------------

static const gdouble ST_ = 1.1; //Stable treshold
static const gdouble OT_ = 2.;  //Overused treshold
static const gdouble DT_ = 1.5; //Down Treshold

static void
sndrate_distor_finalize (
    GObject * object);

//--------------------UTILITIES-----------------------
//static void
//_refresh_targets(
//    SendingRateDistributor* this,
//    Subflow *subflow);


static void
_time_update_preparation(
    SendingRateDistributor* this);

static void
_time_update_correction(
    SendingRateDistributor* this);

static void
_time_update_requestion(
    SendingRateDistributor* this);

static void
_time_update_compilation(
    SendingRateDistributor* this);

static void
_refresh_available_ids(
    SendingRateDistributor* this);


//--------------------MEASUREMENTS-----------------------
static Measurement*
_mt0(Subflow* this);

static Measurement*
_mt1(Subflow* this);

//static Measurement*
//_mt2(Subflow* this);

static Measurement*
_m_step(Subflow *this);

//----------------------STATES-------------------

static State
_check_overused(
    SendingRateDistributor *this,
    Subflow *subflow);

static State
_check_stable(
    SendingRateDistributor *this,
    Subflow *subflow);

static State
_check_monitored(
    SendingRateDistributor *this,
    Subflow *subflow);

//-----------------------ACTIONS------------------
static void
_transit_to(
    SendingRateDistributor *this,
    Subflow *subflow,
    State target);

static void
_disable_controlling(
    Subflow* subflow,
    guint disable_ticks);

//#define _disable_monitoring(subflow) _setup_monitoring(subflow, 0);

#define _disable_monitoring(subflow) _setup_monitoring(subflow, 0)
static void
_setup_monitoring(
    Subflow *subflow,
    guint interval);

static gboolean
_is_monitoring_done(
        SendingRateDistributor *this,
        Subflow *subflow);

static gboolean
_enable_monitoring(
        SendingRateDistributor *this,
        Subflow *subflow);

//static void
//_reset_monitoring(
//        SendingRateDistributor *this,
//        Subflow *subflow);

static guint32
_action_undershoot(
    SendingRateDistributor *this,
    Subflow *subflow);
//

static guint32
_action_bounce_back(
    SendingRateDistributor *this,
    Subflow *subflow);

static guint32
_action_bounce_up(
    SendingRateDistributor *this,
    Subflow *subflow);

//
//inline static
//gdouble sqr(gdouble x) {
//    return x*x;
//}

//source: http://stackoverflow.com/questions/5083465/fast-efficient-least-squares-fit-algorithm-in-c
//
//static gint linreg(gint n,
//                   const gdouble x[],
//                   const gdouble y[],
//                   gdouble* m,
//                   gdouble* b,
//                   gdouble* r);
//
#define _get_subflow(this, n) ((Subflow*)(this->subflows + n * sizeof(Subflow)))


#define foreach_subflows(this, i, subflow) \
  for(i=0, subflow = _get_subflow(this, this->available_ids[0]); i < this->available_ids_length; subflow = _get_subflow(this,  this->available_ids[++i]))
#define _get_next_sending_rate(subflow) \
  (subflow->sending_rate + subflow->requested_bytes - subflow->supplied_bytes)
//static Subflow*
//_get_subflow(
//    SendingRateDistributor *this,
//    guint8 id);
//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------

void
sndrate_distor_class_init (SendingRateDistributorClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = sndrate_distor_finalize;

  GST_DEBUG_CATEGORY_INIT (sndrate_distor_debug_category, "sndrate_distor", 0,
      "MpRTP Sending Rate Distributor");

//  _mprtp_media_rate_utilization_signal =
//      g_signal_new ("mprtp-media-rate-utilization",
//                    G_TYPE_FROM_CLASS (klass),
//                    G_SIGNAL_RUN_LAST,
//                    G_STRUCT_OFFSET (SendingRateDistributorClass, media_rate_utilization),
//                    NULL, NULL, g_cclosure_marshal_generic, G_TYPE_BOOLEAN, 1,
//                    G_TYPE_INT);

}

void
sndrate_distor_finalize (GObject * object)
{
  SendingRateDistributor * this;
  this = SNDRATEDISTOR(object);
  g_object_unref(this->sysclock);
  g_free(this->subflows);
  while(!g_queue_is_empty(this->free_ids)){
    g_free(g_queue_pop_head(this->free_ids));
  }
}


void
sndrate_distor_init (SendingRateDistributor * this)
{
  gint i;
  this->sysclock = gst_system_clock_obtain();
  this->free_ids = g_queue_new();
  this->controlled_num = 0;
  this->subflows = g_malloc0(sizeof(Subflow)*SNDRATEDISTOR_MAX_NUM);
  for(i=0; i<SNDRATEDISTOR_MAX_NUM; ++i){
    _get_subflow(this, i)->id = i;
    _get_subflow(this, i)->available = FALSE;
//    _get_subflow(this, i)->joint_subflow_ids[i] = 1;
  }
}


SendingRateDistributor *make_sndrate_distor(SignalRequestFunc signal_request,
                                            gpointer controller)
{
  SendingRateDistributor *result;
  result = g_object_new (SNDRATEDISTOR_TYPE, NULL);
  result->signal_request = signal_request;
  result->signal_controller = controller;
  result->target_media_rate = 128000;
//  result->target_media_rate = 64000;
  result->actual_media_rate = result->target_media_rate;
  return result;
}

guint8 sndrate_distor_request_id(SendingRateDistributor *this,
                                 MPRTPSPath *path,
                                 guint32 sending_rate)
{
  guint8 result = 0;
  guint8* id;
  if(this->controlled_num > SNDRATEDISTOR_MAX_NUM){
    g_error("TOO MANY SUBFLOW IN CONTROLLER. ERROR!");
    goto exit;
  }
  if(g_queue_is_empty(this->free_ids)){
    result = this->max_id;
    id = &result;
    ++this->max_id;
    goto done;
  }
  id = g_queue_pop_head(this->free_ids);
  result = *id;
  g_free(id);
done:
  //Reset subflow
//  memset(_get_subflow(this, *id), 0, sizeof(Subflow));
  _get_subflow(this, *id)->id = *id;
  _get_subflow(this, *id)->available = TRUE;
  _get_subflow(this, *id)->sending_rate = sending_rate;
  _get_subflow(this, *id)->target_rate = sending_rate;
  _get_subflow(this, *id)->path = g_object_ref(path);
  if(!_get_subflow(this, *id)->delays){
    _get_subflow(this, *id)->delays = make_percentiletracker(256, 80);
    if(*id == 0)
      _get_subflow(this, *id)->goodputs = make_percentiletracker_debug(32, 50);
    else
      _get_subflow(this, *id)->goodputs = make_percentiletracker(32, 50);
    percentiletracker_set_treshold(_get_subflow(this, *id)->delays, 30 * GST_SECOND);
    percentiletracker_set_treshold(_get_subflow(this, *id)->goodputs, 30 * GST_SECOND);
    _get_subflow(this, *id)->falls = make_nlms(10, .01, 1.);
  }
  else{
      percentiletracker_reset(_get_subflow(this, *id)->delays);
      percentiletracker_reset(_get_subflow(this, *id)->goodputs);
  }
  _get_subflow(this, *id)->disable_controlling = 0;
  _get_subflow(this, *id)->measured = FALSE;
  memset(_get_subflow(this, *id)->measurements, 0, sizeof(Measurement) * 3);
  _get_subflow(this, *id)->measurements_index = 0;
  _get_subflow(this, *id)->monitoring_time = 0;
  _get_subflow(this, *id)->monitoring_interval = 0;
  _transit_to(this, _get_subflow(this, *id), STATE_STABLE);
  _disable_controlling(_get_subflow(this, *id), 2);
  ++this->controlled_num;
  _refresh_available_ids(this);
exit:
  return result;
}

void sndrate_distor_remove_id(SendingRateDistributor *this, guint8 id)
{
  guint8 *free_id;
  if(!_get_subflow(this, id)->available) goto done;
  free_id = g_malloc(sizeof(guint8));
  *free_id = id;
  if(_get_subflow(this, id)->monitoring_interval){
    mprtps_path_set_monitor_interval(_get_subflow(this, id)->path, 0);
  }
  g_object_unref(_get_subflow(this, id)->path);
  _setup_monitoring(_get_subflow(this, id), 0);
  _get_subflow(this, id)->available = FALSE;
  g_queue_push_tail(this->free_ids, free_id);
  --this->controlled_num;
  _refresh_available_ids(this);
done:
  return;
}



void sndrate_distor_measurement_update(SendingRateDistributor *this,
                                       guint8 id,
                                       RRMeasurement *measurement)
{
  Subflow *subflow;

  subflow = _get_subflow(this, id);
  _mt0(subflow)->state = subflow->state;
  _m_step(subflow);
  _mt0(subflow)->state = _mt1(subflow)->state;

  if(subflow->state != STATE_OVERUSED){
    percentiletracker_add(subflow->delays, measurement->min_delay);
    percentiletracker_add(subflow->delays, measurement->median_delay);
  }
  _mt0(subflow)->corrh_owd = (gdouble)measurement->median_delay / (gdouble) percentiletracker_get_stats(subflow->delays, NULL, NULL, NULL);
  _mt0(subflow)->delay = measurement->median_delay;
  _mt0(subflow)->corrd = (gdouble)_mt1(subflow)->delay / (gdouble)_mt0(subflow)->delay;
  _mt0(subflow)->discard = measurement->late_discarded_bytes;
  _mt0(subflow)->discard_rate = (gdouble)measurement->late_discarded_bytes / measurement->goodput;
  _mt0(subflow)->lost = measurement->lost;
  _mt0(subflow)->goodput = measurement->goodput;
  _mt0(subflow)->receiver_rate = measurement->receiver_rate;
  _mt0(subflow)->jitter = measurement->jitter;
  _mt0(subflow)->delay_ratio = (gdouble)_mt1(subflow)->delay / (gdouble)_mt0(subflow)->delay;
  _mt0(subflow)->goodput_ratio = (gdouble)_mt0(subflow)->goodput / _mt1(subflow)->goodput;

  if(subflow->state == STATE_OVERUSED)
   percentiletracker_add(subflow->goodputs, subflow->bandwidth_estimation);
  else
   percentiletracker_add(subflow->goodputs, MAX(measurement->goodput,0.));
  subflow->bandwidth_estimation = (gdouble) percentiletracker_get_stats(subflow->goodputs, NULL, NULL, NULL);

//  g_print("S%d update."
//          "State: %d, CorrH: %f, GP: %f, J: %d "
//          "PiT: %hu, L:%d, D: %d, RR: %u "
//          "SR: %d DR: %f GR: %f\n",
//          subflow->id,
//          _mt0(subflow)->state,
//          _mt0(subflow)->corrh_owd,
//          _mt0(subflow)->goodput,
//          _mt0(subflow)->jitter,
//          _mt0(subflow)->PiT,
//          _mt0(subflow)->lost,
//          _mt0(subflow)->discard,
//          _mt0(subflow)->receiver_rate,
//          subflow->sending_rate,
//          _mt0(subflow)->delay_ratio,
//          _mt0(subflow)->goodput_ratio);

  return;
}

void sndrate_distor_extract_stats(SendingRateDistributor *this,
                                  guint8 id,
                                  guint64 *median_delay,
                                  gint32  *sending_rate,
                                  gdouble *bandwidth_estimation,
                                  gdouble *goodput,
                                  gdouble *bandwidth_estimation_error)
{
  Subflow *subflow;
  subflow = _get_subflow(this, id);
  if(!subflow->available) return;
  if(median_delay)
    *median_delay = percentiletracker_get_stats(subflow->delays, NULL, NULL, NULL);
  if(sending_rate)
    *sending_rate = subflow->sending_rate;
  if(bandwidth_estimation)
    *bandwidth_estimation = subflow->bandwidth_estimation;
  if(goodput)
    *goodput = _mt0(subflow)->goodput;
  if(bandwidth_estimation_error)
    *bandwidth_estimation_error = subflow->bandwidth_estimation_error;

}

void sndrate_distor_time_update(SendingRateDistributor *this)
{
  //1. Initialize and reset variable for updating
  _time_update_preparation(this);

  //2. mitigate the effect of byte requesting and supplying by
  //moving the bytes between the subflows
  _time_update_correction(this);

  //3. Requesting the media source for new media rate
  _time_update_requestion(this);

  //4. Applying the new rate on the rate distributor
  _time_update_compilation(this);

  return;
}


void _time_update_preparation(SendingRateDistributor* this)
{
  gint i;
  Subflow *subflow;

  this->supplied_bytes = 0;
  this->requested_bytes = 0;
  this->movable_bytes = 0;

  foreach_subflows(this, i, subflow)
  {
    subflow->requested_bytes = 0;
    subflow->supplied_bytes = 0;
    subflow->movable_bytes = 0;

    subflow->overused_history<<=1;
    if(0 < subflow->target_rate && subflow->target_rate < subflow->sending_rate){
      subflow->movable_bytes=subflow->sending_rate-subflow->target_rate;
      this->movable_bytes+=subflow->movable_bytes;
    }
    if(subflow->disable_controlling > 0){
      --subflow->disable_controlling;
      continue;
    }
//    _refresh_targets(this, subflow);
    subflow->state = subflow->controlling(this, subflow);
    this->supplied_bytes+=subflow->supplied_bytes;
    this->requested_bytes+=subflow->requested_bytes;
  }



}

void _time_update_correction(SendingRateDistributor* this)
{
  Subflow *subflow, *target, *source;
  gint i,j;
  gint32 total = 0, moved_bytes = 0;
  gdouble weight;

  if(this->greedy || 0 == this->supplied_bytes) goto movability;

  foreach_subflows(this, i, subflow)
  {
    if(subflow->overused_history > 0) continue;
    total += subflow->sending_rate;
  }

  if(0 == total) goto movability;

  foreach_subflows(this, i, subflow){
    if(subflow->overused_history > 0) continue;
    weight = (gdouble)subflow->sending_rate / (gdouble)total;
    subflow->requested_bytes += weight * (gdouble) this->supplied_bytes;
  }
  this->supplied_bytes = 0;

movability:
  if(!this->requested_bytes || !this->movable_bytes) goto exit;
  foreach_subflows(this, i, source){
    if(source->movable_bytes == 0) continue;
    foreach_subflows(this, j, target){
      if(target->requested_bytes == 0) continue;
      if(target->requested_bytes <= source->movable_bytes){
        source->movable_bytes -= moved_bytes = target->requested_bytes;
        source->supplied_bytes+=target->requested_bytes;
      }else{
        source->supplied_bytes+= moved_bytes = source->movable_bytes;
        source->movable_bytes = 0;
      }
    }
  }

exit:
  return;
}

void _time_update_requestion(SendingRateDistributor* this)
{
  UtilizationReport ur;
  gint32 desired_media_rate;
  gint i;
  Subflow *subflow;

  desired_media_rate = (gint32)this->actual_media_rate + this->requested_bytes - this->supplied_bytes;
  if(desired_media_rate < 0){
    g_warning("Ó basszus.");
    desired_media_rate = 6400;
  }
  ur.desired_rate = desired_media_rate;
  ur.actual_target = this->target_media_rate;

  this->signal_request(this->signal_controller, &ur);

  if(ur.actual_rate == 0) {
    g_warning("The actual media rate must be greater than 0");
    ur.actual_rate = desired_media_rate;
  }

  if(0 < ur.desired_target && ur.desired_target < ur.actual_rate){
    ur.desired_target = ur.actual_rate;
  }
  this->target_media_rate = ur.desired_target;
  this->greedy = ur.greedy;
  this->actual_media_rate = ur.actual_rate;

  foreach_subflows(this, i, subflow){
    subflow->target_weight = ur.subflows[subflow->id].target_weight;
  }

}

void _time_update_compilation(SendingRateDistributor* this)
{
  gint i;
  Subflow *subflow;

  foreach_subflows(this, i, subflow)
  {
    subflow->sending_rate = _get_next_sending_rate(subflow);
    subflow->weight = (gdouble) subflow->sending_rate / this->actual_media_rate;
    subflow->measured=FALSE;
    subflow->target_rate = 0;
    if(0. < subflow->target_weight){
      if(0 < this->target_media_rate)
        subflow->target_rate = subflow->target_weight * this->target_media_rate;
      else if(subflow->target_weight < subflow->weight * .95 || subflow->target_weight < subflow->weight  * 1.05)
        subflow->target_rate = subflow->target_weight * this->actual_media_rate;
      else
        subflow->target_weight = 0.;
    }
  }

}


guint32 sndrate_distor_get_sending_rate(SendingRateDistributor *this, guint8 id)
{
  return _get_subflow(this, id)->sending_rate;
}

gboolean sndrate_distor_congestion_event(SendingRateDistributor *this, guint8 id)
{
  return _get_subflow(this, id)->state == STATE_OVERUSED &&
         _mt1(_get_subflow(this, id))->state != STATE_OVERUSED;
}

gboolean sndrate_distor_settlemen_event(SendingRateDistributor *this, guint8 id)
{
  return _get_subflow(this, id)->state == STATE_STABLE &&
         _mt1(_get_subflow(this, id))->state != STATE_STABLE;
}
//
//void _refresh_targets(SendingRateDistributor* this, Subflow *subflow)
//{
//  gdouble rate;
//  subflow->step_rate = 0;
//  if(0 == subflow->target_rate) return;
//  subflow->step_rate = subflow->target_rate;
//  for(rate = (gdouble) subflow->target_rate / (gdouble)subflow->sending_rate;
//      2. < rate;
//      rate = (gdouble) subflow->step_rate / (gdouble)subflow->sending_rate)
//  {
//    subflow->step_rate*=.5;
//  }
//  subflow->step_rate = subflow->target_rate;
//}

void _refresh_available_ids(SendingRateDistributor* this)
{
  gint id;
  Subflow *subflow;
  this->available_ids_length = 0;
  for(id=0; id < SNDRATEDISTOR_MAX_NUM; ++id){
    subflow = _get_subflow(this, id);
    if(!subflow->available) continue;
    this->available_ids[this->available_ids_length++] = subflow->id;
  }
}

Measurement* _mt0(Subflow* this)
{
  return &this->measurements[this->measurements_index];
}

Measurement* _mt1(Subflow* this)
{
  guint8 index;
  if(this->measurements_index == 0) index = MEASUREMENT_LENGTH-1;
  else index = this->measurements_index-1;
  return &this->measurements[index];
}

//
//Measurement* _mt2(Subflow* this)
//{
//  guint8 index;
//  if(this->measurements_index == 1) index = MEASUREMENT_LENGTH-1;
//  else if(this->measurements_index == 0) index = MEASUREMENT_LENGTH-2;
//  else index = this->measurements_index-2;
//  return &this->measurements[index];
//}

Measurement* _m_step(Subflow *this)
{
  if(++this->measurements_index == MEASUREMENT_LENGTH){
    this->measurements_index = 0;
  }
  memset(&this->measurements[this->measurements_index], 0, sizeof(Measurement));
  return _mt0(this);
}

State
_check_overused(
    SendingRateDistributor *this,
    Subflow *subflow)
{
  if(_mt0(subflow)->discard_rate > .2){
    if(_mt1(subflow)->state == STATE_OVERUSED){
      subflow->supplied_bytes += _action_undershoot(this, subflow);
      _disable_monitoring(subflow);
      _disable_controlling(subflow, 1);
      goto done;
    }
  }
  if(_mt0(subflow)->corrh_owd > OT_){
    if(_mt0(subflow)->corrh_owd > _mt1(subflow)->corrh_owd){
      subflow->supplied_bytes += _action_undershoot(this, subflow);
      _disable_monitoring(subflow);
      _disable_controlling(subflow, 1);
      goto done;
    }
    if(_mt1(subflow)->state != STATE_OVERUSED) goto done;
    subflow->supplied_bytes += _action_undershoot(this, subflow);
    _disable_monitoring(subflow);
    goto done;
  }

  if(_mt1(subflow)->state != STATE_OVERUSED) goto done;

  subflow->requested_bytes += _action_bounce_back(this, subflow);
  _transit_to(this, subflow, STATE_STABLE);
done:
  return subflow->state;
}


State
_check_stable(
    SendingRateDistributor *this,
    Subflow *subflow)
{
  if(_mt0(subflow)->discard_rate > .1 || _mt0(subflow)->corrh_owd > DT_){
       subflow->supplied_bytes += _action_undershoot(this, subflow);
       _transit_to(this, subflow, STATE_OVERUSED);
       _disable_controlling(subflow, 1);
       goto done;
  }

  if(_mt0(subflow)->corrh_owd > ST_)
  {
     if(_mt1(subflow)->state != STATE_STABLE)
       goto done;
     subflow->supplied_bytes += _action_undershoot(this, subflow);
     _transit_to(this, subflow, STATE_OVERUSED);
     _disable_controlling(subflow, 1);
     goto done;
  }


  if(_mt0(subflow)->corrh_owd > 1. || _mt1(subflow)->corrh_owd > 1.)
  {
    ++subflow->turning_point;
    subflow->monitoring_disabled = 1;
    goto done;
  }

  if(0 < subflow->monitoring_disabled){
    --subflow->monitoring_disabled;
    goto done;
  }

  if(_enable_monitoring(this, subflow)){
    _transit_to(this, subflow, STATE_MONITORED);
  }

done:
  return subflow->state;
}

State
_check_monitored(
    SendingRateDistributor *this,
    Subflow *subflow)
{

//  g_print("S%d: STATE MONITORED\n", subflow->id);

  if(_mt0(subflow)->corrh_owd > ST_ || _mt0(subflow)->discard_rate > .1){
      subflow->supplied_bytes +=_action_undershoot(this, subflow);
      _transit_to(this, subflow, STATE_OVERUSED);
      _disable_controlling(subflow, 1);
      _disable_monitoring(subflow);
    goto done;
  }

  if(_mt0(subflow)->discard){
    ++subflow->turning_point;
    _disable_monitoring(subflow);
    _transit_to(this, subflow, STATE_STABLE);
    goto done;
  }

  if( 1.05 < _mt0(subflow)->corrh_owd){
    ++subflow->turning_point;
    _disable_monitoring(subflow);
    _transit_to(this, subflow, STATE_STABLE);
    goto done;
  }

  if(_is_monitoring_done(this, subflow)){
    //increase
    _disable_monitoring(subflow);
    subflow->requested_bytes +=_action_bounce_up(this, subflow);
    _transit_to(this, subflow, STATE_STABLE);
  }
done:
  return subflow->state;
}


//------------------------ACTIONS-----------------------------

void
_transit_to(
    SendingRateDistributor *this,
    Subflow *subflow,
    State target)
{
  if(subflow->state == STATE_MONITORED && target != subflow->state){
    --this->monitored;
  }

  if(subflow->state == STATE_OVERUSED && target != subflow->state){
    --this->overused;
  }


  switch(target){
    case STATE_OVERUSED:
      mprtps_path_set_congested(subflow->path);
      ++this->overused;
      subflow->controlling = _check_overused;
      subflow->monitoring_interval = 0;
      subflow->overused_history+=1;
    break;
    case STATE_STABLE:
      mprtps_path_set_non_congested(subflow->path);
      subflow->controlling = _check_stable;

    break;
    case STATE_MONITORED:
      ++this->monitored;
      subflow->controlling = _check_monitored;
      subflow->monitoring_time = 0;
    break;
  }
  subflow->state = target;
}


void _disable_controlling(Subflow* subflow, guint disable_ticks)
{
  if(!subflow->available) goto done;
  subflow->disable_controlling += disable_ticks;
done:
  return;
}


void _setup_monitoring(Subflow *subflow, guint interval)
{
  if(!subflow->available) goto done;
  subflow->monitoring_time = 0;
  mprtps_path_set_monitor_interval(subflow->path, interval);
done:
  return;
}

gboolean _is_monitoring_done(
        SendingRateDistributor *this,
        Subflow *subflow)
{
  if(!subflow->available) goto yes;
  if(1 < subflow->monitoring_interval) goto yes;
  if(1 < ++subflow->monitoring_time) goto yes;

  return FALSE;
yes:
  return TRUE;
}



gboolean _enable_monitoring(
        SendingRateDistributor *this,
        Subflow *subflow)
{
  gdouble rate;
  gdouble target;
  gdouble actual;
  subflow->monitoring_interval = 0;
  if(!subflow->available) goto exit;

  if(subflow->sending_rate < subflow->bandwidth_estimation){
    target = subflow->bandwidth_estimation * 1.1;
    actual = subflow->sending_rate;
    goto determine;
  }
  if(0 < subflow->target_rate){
    if(subflow->target_rate <= subflow->sending_rate) {
      goto exit;
    }
    target = subflow->target_rate;
    actual = subflow->sending_rate;
    goto determine;
  }
  if(0 < this->target_media_rate){
    if(this->target_media_rate <= this->actual_media_rate){
      goto exit;
    }
    target = this->target_media_rate - subflow->sending_rate;
    actual = this->actual_media_rate;
    goto determine;
  }

  subflow->monitoring_interval = 7;
  goto determined;

determine:
  rate = target / actual;
  if(rate > 2.) subflow->monitoring_interval = 2;
  else if(rate > 1.5) subflow->monitoring_interval = 3;
  else if(rate > 1.25) subflow->monitoring_interval = 4;
  else if(rate > 1.2) subflow->monitoring_interval = 5;
  else if(rate > 1.16) subflow->monitoring_interval = 6;
  else if(rate > 1.14) subflow->monitoring_interval = 7;
  else if(rate > 1.12) subflow->monitoring_interval = 8;
  else if(rate > 1.11) subflow->monitoring_interval = 9;
  else if(rate > 1.10) subflow->monitoring_interval = 10;
  else if(rate > 1.09) subflow->monitoring_interval = 11;
  else if(rate > 1.06) subflow->monitoring_interval = 12;
  else if(rate > 1.03) subflow->monitoring_interval = 13;
  else subflow->monitoring_interval = 14;

determined:
  subflow->turning_point = MIN(subflow->turning_point, 3);
  if(0 < subflow->turning_point){
    if(subflow->monitoring_interval == 2)
      subflow->monitoring_interval*=pow(2., subflow->turning_point);
    else if(subflow->monitoring_interval < 5)
      subflow->monitoring_interval*=subflow->turning_point + 1;
    else if(subflow->monitoring_interval < 10)
      subflow->monitoring_interval+=subflow->turning_point * 2;
    else
      subflow->monitoring_interval=14;
  }
  subflow->monitoring_interval = MIN(14, subflow->monitoring_interval);
exit:
  _setup_monitoring(subflow, subflow->monitoring_interval);
  return subflow->monitoring_interval > 0;
}


//void _reset_monitoring(
//        SendingRateDistributor *this,
//        Subflow *subflow)
//{
//  if(!subflow->available) goto done;
//  subflow->monitoring_interval = 14;
//done:
//  _setup_monitoring(subflow, subflow->monitoring_interval);
//  return;
//}


guint32 _action_undershoot(
    SendingRateDistributor *this,
    Subflow *subflow)
{
  guint32 supplied_bytes = 0;
  gint32 min_rate;
  gdouble shooter;
  min_rate = MAX(6400, subflow->target_rate * .05);
  if(1. < _mt0(subflow)->goodput_ratio && 1. < _mt0(subflow)->delay_ratio)
    shooter = 1.;
  else if(0. < _mt0(subflow)->goodput_ratio && 0. < _mt0(subflow)->delay_ratio)
    shooter = MIN(_mt0(subflow)->goodput_ratio, _mt0(subflow)->delay_ratio);
  else if(0. < _mt0(subflow)->goodput_ratio)
    shooter = _mt0(subflow)->goodput_ratio;
  else if(0. < _mt0(subflow)->delay_ratio)
    shooter = _mt0(subflow)->delay_ratio;
  else
    shooter = .707;

  if(1. < shooter){
      //because of discard rate
      if(_mt0(subflow)->discard_rate < .5)
        shooter = .303;
      else
        shooter = .505;
  }
  else if(shooter < .25) shooter*=.5;
  else if(shooter < .5) shooter *= .5;
  else if(shooter < .75) shooter *= .5;
  else if(shooter < 1.) shooter *= .75;

  supplied_bytes = subflow->sending_rate * (1.-shooter);
  if(subflow->sending_rate - supplied_bytes < min_rate){
    if(min_rate < subflow->sending_rate)
      supplied_bytes = subflow->sending_rate - min_rate;
    else
      supplied_bytes = 0;
  }
  subflow->falling_point = subflow->sending_rate;
  subflow->bounce_point = _mt0(subflow)->goodput;

//  g_print("Undershoot on S%d. SR: %d SB:%u (%f)\n",
//          subflow->id, subflow->sending_rate, supplied_bytes, shooter);
  return supplied_bytes;
}




guint32 _action_bounce_back(
    SendingRateDistributor *this,
    Subflow *subflow)
{
  gdouble bounce_point_mt0;
  gdouble bounce_point_mt1;
  gdouble bounce_point;
  guint32 requested_bytes = 0;

  bounce_point_mt0 = MIN(_mt0(subflow)->delay_ratio, _mt0(subflow)->goodput_ratio);
  bounce_point_mt1 = MIN(_mt1(subflow)->delay_ratio, _mt1(subflow)->goodput_ratio);
  bounce_point = MAX(bounce_point_mt0, bounce_point_mt1);
  bounce_point = MAX(1., bounce_point);
  bounce_point = MIN(bounce_point, 2.) - 1.;
  requested_bytes = MAX(bounce_point * subflow->sending_rate, subflow->bounce_point);
  requested_bytes *= .9;
//  g_print("BounceBack on S%d. SR: %d RB:%u\n", subflow->id, subflow->sending_rate, requested_bytes);
  return requested_bytes;
}


guint32 _action_bounce_up(
    SendingRateDistributor *this,
    Subflow *subflow)
{
  guint32 requested_bytes = 0;
  requested_bytes = subflow->sending_rate * (1./(gdouble) subflow->monitoring_interval);
  if(0 < subflow->turning_point) --subflow->turning_point;
//  g_print("BounceUp on S%d. SR: %d RB:%u\n", subflow->id, subflow->sending_rate, requested_bytes);
  return requested_bytes;
}

