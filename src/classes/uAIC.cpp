/*
 * File:   AIC.cpp
 * Author: Corrado Pezzato, TU Delft, DCSC
 *
 * Created on Sept. 28th, 2021
 *
 * Class to perform active inference contro
 * Definition of the methods contained in AIC.h
 *
 */

#include "uAIC.h"

  // Constructor which takes as argument the publishers and initialises the private ones in the class
  uAIC::uAIC(int whichRobot){
    // Initialize the variables for thr uAIC
    uAIC::initVariables();
      // Torque publisher
      torque_pub = nh.advertise<std_msgs::Float64MultiArray>("/joint_torques_des", 20);
      // Listener joint states
      sensorSub = nh.subscribe("/joint_states", 1, &uAIC::jointStatesCallback, this);

      // Listener to goals
      goal_mu_dSub = nh.subscribe("/desired_state", 5, &uAIC::setDesiredState, this);


  }
  uAIC::~uAIC(){}

  // TODO change to reference.msg
  // Method to set the current goal from topic, this is the input to the controller
  void uAIC::setDesiredState(const unbiased_aic::reference::ConstPtr& msg){
    for( int i = 0; i < 7; i++ ) {
      mu_d(i) = msg->ref_position.data[i];
      mu_p_d(i) = msg->ref_velocity.data[i];
      // mu_p_d(i) = 0.0;
    }
  }

  void   uAIC::jointStatesCallback(const sensor_msgs::JointState::ConstPtr& msg)
  {
    // Save joint values
    for( int i = 0; i < 7; i++ ) {
      jointPos(i) = msg->position[i];
      jointVel(i) = msg->velocity[i];
    }
    // If this is the first time we read the joint states then we set the current beliefs
    if (dataReceived == 0){
      // Track the fact that the encoders published
      dataReceived = 1;
      // The first time we retrieve the position we define the initial beliefs about the states
      mu = jointPos;
      mu_p = jointVel;
      mu_past = mu;
      mu_p_past = mu_p;
    }
  }

  void   uAIC::initVariables(){

    // Support variable
    dataReceived = 0;

    // Precision matrices (first set them to zero then populate the diagonal)
    SigmaP_yq0 = Eigen::Matrix<double, 7, 7>::Zero();
    SigmaP_yq1 = Eigen::Matrix<double, 7, 7>::Zero();
    SigmaP_mu = Eigen::Matrix<double, 7, 7>::Zero();
    SigmaP_muprime = Eigen::Matrix<double, 7, 7>::Zero();
    K_p = Eigen::Matrix<double, 7, 7>::Zero();
    K_d = Eigen::Matrix<double, 7, 7>::Zero();
    K_i = Eigen::Matrix<double, 7, 7>::Zero();

    // Begin Tuning parameters of u-AIC
    //---------------------------------------------------------------
    // Variances associated with the beliefs and the sensory inputs
    ROS_INFO("Setting parameters from parameter space");
    nh.getParam("var_mu", var_mu);
    nh.getParam("var_muprime", var_muprime);
    nh.getParam("var_q", var_q);
    nh.getParam("var_qdot", var_qdot);

    // Controller values, diagonal elements of the gain matrices for the PID like control law
    nh.getParam("k_p", k_p);
    nh.getParam("k_d", k_d);
    nh.getParam("k_i", k_i);
    nh.getParam("k_mu", k_mu);

    I_gain <<  0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02;

    // Learning rates for the gradient descent (found that a ratio of 60 works good)

    // End tuning parameters
    //---------------------------------------------------------------

    // Populate matrices
    for( int i = 0; i < SigmaP_yq0.rows(); i = i + 1 ) {
      SigmaP_yq0(i,i) = 1/var_q;
      SigmaP_yq1(i,i) = 1/var_qdot;
      SigmaP_mu(i,i) = 1/var_mu;
      SigmaP_muprime(i,i) = 1/var_muprime;
      K_p(i,i) = k_p;
      K_d(i,i) = k_d;
      K_i(i,i) = k_i;
    }

    // Last joint
    K_p(6,6) = 0.3*k_p;
    K_d(6,6) = 0.3*k_d;
    K_i(6,6) = 0.3*k_i;

    // Initialize control actions
    u << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;

    // Initialize prior beliefs about the second ordet derivatives of the states of the robot
    mu_pp << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;

    // Integration step
    h = 0.001;
    // Resize the data for the published message
    torque_command.data.resize(7);

  }

  void uAIC::minimiseF(){

    // Unbiased uAIC
    mu_dot = - k_mu*(-SigmaP_yq0*(jointPos-mu) + SigmaP_mu*(mu - (mu_past + h*mu_p_past)));
    mu_dot_p = - k_mu*(-SigmaP_yq1*(jointVel-mu_p) + SigmaP_muprime*(mu_p-mu_p_past));

    // Save current value of the belief to use in the next iteration as previous value
    mu_past = mu;
    mu_p_past = mu_p;

    // Belifs update
    mu = mu + h*mu_dot;             // Belief about the position
    mu_p = mu_p + h*mu_dot_p;       // Belief about motion of mu
    // mu_pp = mu_pp + h*mu_dot_pp;    // Belief about motion of mu'

    // Set curret values for next ieration
    I_gain = I_gain + mu_d-mu;

    // Calculate and send control actions
    uAIC::computeActions();

  }

  void   uAIC::computeActions(){
    // Compute control actions through gradient descent of F, AIC
    //u = u-h*k_a*(SigmaP_yq1*(jointVel-mu_p)+SigmaP_yq0*(jointPos-mu));

    // Unbiased uAIC
    u = K_p*(mu_d-mu) + K_d*(mu_p_d-mu_p) + K_i*(I_gain);

    // Set the toques from u and publish
    for (int i=0;i<7;i++){
       torque_command.data[i] = u(i);
    }

    // Publishing
    torque_pub.publish(torque_command);
  }

  // Method to control if the joint states have been received already,
  // used in the main function
  int uAIC::dataReady(){
    if(dataReceived==1)
      return 1;
    else
      return 0;
  }

  // Method to set the desired position from script, used in the main to initialize the arm to a central pose and keep it there
  void uAIC::setGoal(std::vector<double> desiredPos){
    for(int i=0; i<desiredPos.size(); i++){
      mu_d(i) = desiredPos[i];
      mu_p_d(i) = 0;
    }
  }

  void uAIC::setGoalCurrentState(){
    for(int i=0; i<7; i++){
      mu_d(i) = jointPos(i);
      mu_p_d(i) = 0;
//      std::cout << jointPos(i) << "\n";
    }
  }

  std_msgs::Float64MultiArray  uAIC::getSPE(){
    return(SPE);
  }
