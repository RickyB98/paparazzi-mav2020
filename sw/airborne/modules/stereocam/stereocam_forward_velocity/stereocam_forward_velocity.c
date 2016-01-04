/*
 * Copyright (C) Roland
 *
 * This file is part of paparazzi
 *
 */
/**
 * @file "modules/follow_me/follow_me.c"
 * @author Roland
 * follows a person on the stereo histogram image.
 * It searches for the highest peak and adjusts its roll and pitch to hover at a nice distance.
 */

#include "modules/stereocam/stereocam_forward_velocity/stereocam_forward_velocity.h"
#include "modules/stereocam/stereocam.h"
#include "state.h"
#include "navigation.h"
#include "subsystems/abi.h"
#include "firmwares/rotorcraft/guidance/guidance_h.h"
#include "subsystems/datalink/telemetry.h"

#define AVERAGE_VELOCITY 0
// Know waypoint numbers and blocks
#include "generated/flight_plan.h"
 float ref_pitch=0.0;
 float ref_roll=0.0;
void stereocam_forward_velocity_init()
{

}
void array_pop(float *array, int lengthArray)
{
  int index;
  for (index = 1; index < lengthArray; index++) {
    array[index - 1] = array[index];
  }
}
int turnFactors[]={300,300,300,200,100,100,100,100};
int countFactorsTurning=6;
int indexTurnFactors=0;


int disparity_velocity_step = 0;
int disparity_velocity_max_time = 500;
int distancesRecorded = 0;
int timeStepsRecorded = 0;
int velocity_disparity_outliers = 0;
float distancesHistory[500];
float timeStepHistory[500];
#define LENGTH_VELOCITY_HISTORY 6
float velocityHistory[LENGTH_VELOCITY_HISTORY];
int indexVelocityHistory=0;
float sumVelocities=0.0;

float sumHorizontalVelocities=0.0;
uint8_t GO_FORWARD=0;
uint8_t TURN=1;
uint8_t STABILISE=2;
uint8_t INIT_FORWARD=3;
uint8_t current_state=2;
int totalStabiliseStateCount = 0;
int totalTurningSeenNothing=0;
float previousLateralSpeed = 0.0;
uint8_t detectedWall=0;
float velocityAverageAlpha = 0.65;
float previousHorizontalVelocity = 0.0;
#define DANGEROUS_CLOSE_DISPARITY 40
#define CLOSE_DISPARITY 33
#define LOW_AMOUNT_PIXELS_IN_DROPLET 20
float ref_disparity_to_keep=40.0;
float pitch_compensation = 0.05;
int initFastForwardCount = 0;
int goForwardXStages=3;
float ref_alt=1.0;
typedef enum{USE_DROPLET,USE_CLOSEST_DISPARITY} something;
something sf = USE_DROPLET;
float heading=0.0;
float calculateForwardVelocity(float distance,float alpha,int MAX_SUBSEQUENT_OUTLIERS,int n_steps_velocity)
{
	    disparity_velocity_step += 1;
	    float new_dist = 0.0;
	    if (distancesRecorded > 0) {
	      new_dist = alpha * distancesHistory[distancesRecorded - 1] + (1 - alpha) * distance;
	    }
	    // Deal with outliers:
	    // Single outliers are discarded, while persisting outliers will lead to an array reset:
	    if (distancesRecorded > 0 && fabs(new_dist - distancesHistory[distancesRecorded - 1]) > 0.5) {
	      velocity_disparity_outliers += 1;
	      if (velocity_disparity_outliers >= MAX_SUBSEQUENT_OUTLIERS) {
	        // The drone has probably turned in a new direction
	        distancesHistory[0] = new_dist;
	        distancesRecorded = 1;

	        timeStepHistory[0] = disparity_velocity_step;
	        timeStepsRecorded = 1;
	        velocity_disparity_outliers = 0;
	      }
	    } else {
	        //append
	      velocity_disparity_outliers = 0;
	      timeStepHistory[timeStepsRecorded] = disparity_velocity_step;
	      distancesHistory[distancesRecorded] = new_dist;
	      distancesRecorded++;
	      timeStepsRecorded++;
	    }

	    //determine velocity (very simple method):
	    float velocityFound = 0.0;
	    if (distancesRecorded > n_steps_velocity) {
	      velocityFound = distancesHistory[distancesRecorded - n_steps_velocity] - distancesHistory[distancesRecorded - 1];
	    }
	    // keep maximum array size:
	    if (distancesRecorded > disparity_velocity_max_time) {
	    	array_pop(distancesHistory, disparity_velocity_max_time);
	    }
	    if (timeStepsRecorded > disparity_velocity_max_time) {
	    	array_pop(timeStepHistory, disparity_velocity_max_time);
	    }
	    return velocityFound;
}
void increase_nav_heading(int32_t *heading, int32_t increment);
void increase_nav_heading(int32_t *heading, int32_t increment)
{
  *heading = *heading + increment;
}
void stereocam_forward_velocity_periodic()
{

  if (stereocam_data.fresh && stereocam_data.len>20) {
//	  if (autopilot_mode != AP_MODE_NAV) {
//
//	  		  struct Int32Eulers *euler = stateGetNedToBodyEulers_i();
//	  		  nav_set_heading_rad(ANGLE_FLOAT_OF_BFP(euler->psi));
//	  	//heading=;
//	  }
    stereocam_data.fresh = 0;
	uint8_t closest = stereocam_data.data[4];

	uint8_t disparitiesInDroplet = stereocam_data.data[5];
    int horizontalVelocity = stereocam_data.data[8]-127;
    int upDownVelocity = stereocam_data.data[9] -127;

    float  BASELINE_STEREO_MM = 60.0;
    float BRANDSPUNTSAFSTAND_STEREO = 118.0 * 6.0 * 2.0;
	float dist = 5.0;
	if (closest > 0) {
	  dist = ((BASELINE_STEREO_MM * BRANDSPUNTSAFSTAND_STEREO / (float)closest)) / 1000;
	}
	float velocityFound = calculateForwardVelocity(dist,0.65, 5,5);

    float guidoVelocityHorStereoboard = horizontalVelocity/100.0;
    float guidoVelocityHor = 0.0;

    // Set the velocity to either the average of the last few velocities, or take the current velocity with alpha times the previous one
    guidoVelocityHor = guidoVelocityHorStereoboard*velocityAverageAlpha + (1-velocityAverageAlpha)*previousHorizontalVelocity;
    sumHorizontalVelocities+=guidoVelocityHor;
    previousHorizontalVelocity= guidoVelocityHorStereoboard;

    int timeStamp = 0;
    //float guidoVelocityZ = upDownVelocity/100.0;
    float guidoVelocityZ=0.0;
    float noiseUs = 0.3f;

    AbiSendMsgVELOCITY_ESTIMATE(STEREO_VELOCITY_ID, timeStamp, velocityFound, guidoVelocityHor,
                                guidoVelocityZ,
                                noiseUs);
	ref_pitch=-.05;
    ref_roll=0.0;
    if(autopilot_mode != AP_MODE_NAV){
    	 ref_alt= -state.ned_pos_f.z;
    }

    float usedIFactor=0.0;
    float differenceD = guidoVelocityHor -previousLateralSpeed;
    previousLateralSpeed=guidoVelocityHor;
    if(current_state==GO_FORWARD){
    	if(sf==USE_CLOSEST_DISPARITY){
			if(closest>DANGEROUS_CLOSE_DISPARITY){
				ref_pitch=0.2;
				detectedWall=1;
			}
			else if(closest>CLOSE_DISPARITY){
				ref_pitch=0.1;
				detectedWall=1;
			}
    	}
    	else{
    		if(disparitiesInDroplet>30){
    			ref_pitch=0.2;
    			detectedWall=1;
    		}
    	}
		float p_gain = 0.2;
		float i_gain = 0.00;
		float d_gain = 0.00;
		float max_roll=0.1;
		float rollToTake = p_gain * guidoVelocityHor+sumVelocities*i_gain - d_gain*differenceD;

		if(rollToTake>max_roll){
			ref_roll=max_roll;
		}
		else if(rollToTake<(-1.0*max_roll)){
			ref_roll=-(1.0*max_roll);
		}
		else{
			ref_roll=rollToTake;
		}

		if(closest < DANGEROUS_CLOSE_DISPARITY && detectedWall){
			totalTurningSeenNothing=0;
			current_state=STABILISE;
			totalStabiliseStateCount=0;
			detectedWall=0;
		}

    }
    else if(current_state==STABILISE){
    	float stab_pitch_pgain=0.015;
    	if(autopilot_mode != AP_MODE_NAV){
    			ref_disparity_to_keep=closest;
    		}
    	float pitchDiff = closest- ref_disparity_to_keep;
    	float pitchToTake = stab_pitch_pgain*pitchDiff;
    	if(pitchToTake>0.1){
    		ref_pitch=0.1;
    	}
    	else if (pitchToTake<-0.1){
    		ref_pitch=-0.1;
    	}
    	else{
    		ref_pitch=pitchToTake;
    	}
//    	if(totalStabiliseStateCount<4){
//			ref_pitch=0.1;
//			totalStabiliseStateCount++;
//		}
//    	else{
//    		current_state=TURN;
//    	}

    	float p_gain = 0.2;
		float i_gain = 0.01;
		float d_gain = 0.05;
		float max_roll=0.1;
		usedIFactor=sumHorizontalVelocities*i_gain;
		float rollToTake = p_gain * guidoVelocityHor+usedIFactor - d_gain*differenceD;

		if(rollToTake>max_roll){
			ref_roll=max_roll;
		}
		else if(rollToTake<(-1.0*max_roll)){
			ref_roll=-(1.0*max_roll);
		}
		else{
			ref_roll=rollToTake;
		}
ref_roll=0.0;
    //	if(guidoVelocityHor<0.5 && guidoVelocityHor>-0.5){
    	//	if(sf==USE_CLOSEST_DISPARITY){
	//			if(closest < CLOSE_DISPARITY){
	//				current_state=TURN;
	//				indexTurnFactors=0;
	//			}
//    		}
//    		else{
//    			if(disparitiesInDroplet<LOW_AMOUNT_PIXELS_IN_DROPLET){
//    				current_state=TURN;
//    				indexTurnFactors=0;
//    			}
//    		}
   // 	}
       }
    else if(current_state==TURN){
    	ref_pitch=0.13;
    	ref_roll=0.1;
    	 heading += 18.0;
    	  if (heading > 360.0)
    	    heading -= 360.0;

    	//increase_nav_heading(&nav_heading,turnFactors[indexTurnFactors]);
    	indexTurnFactors+=1;
    	if(indexTurnFactors>countFactorsTurning){
    		indexTurnFactors = countFactorsTurning;
    	}
    	if(indexTurnFactors > 3){
    		if(sf==USE_CLOSEST_DISPARITY){
    			if(closest<CLOSE_DISPARITY){
					totalTurningSeenNothing++;
					if(totalTurningSeenNothing>2){
						current_state=INIT_FORWARD;
						detectedWall=0;
					}
    			}
    		}
    		else{
    			if(disparitiesInDroplet<LOW_AMOUNT_PIXELS_IN_DROPLET){
    				totalTurningSeenNothing++;
					if(totalTurningSeenNothing>2){
						current_state=INIT_FORWARD;
						detectedWall=0;
					}
    			}
    		}
    	}
    	else{
    		totalTurningSeenNothing=0;
    	}

    }
    else if(current_state==INIT_FORWARD){
    	ref_pitch=-0.25;
    	initFastForwardCount++;
    	if(initFastForwardCount >= goForwardXStages){
    		initFastForwardCount = 0;
    		current_state=GO_FORWARD;
    	}
     }
    else{
    	current_state=GO_FORWARD;
    }
   // nav_set_heading_deg(heading);

    ref_pitch += pitch_compensation;
    DOWNLINK_SEND_STEREO_VELOCITY(DefaultChannel, DefaultDevice, &closest, &disparitiesInDroplet,&dist, &velocityFound,&guidoVelocityHor,&usedIFactor,&current_state);

    DOWNLINK_SEND_REFROLLPITCH(DefaultChannel, DefaultDevice, &ref_roll,&ref_pitch);
//*/
  }
}
