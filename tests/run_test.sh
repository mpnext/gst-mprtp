#!/bin/bash
#VETH0_CMDS="./run_veth0_cc.sh"
VETH0_CMDS="./run_veth0_const.sh"
VETH2_CMDS="./run_veth2_cc.sh"
#VETH2_CMDS="./run_veth2_const.sh"
VETH4_CMDS="./run_veth4_const.sh"
SERVER="./server"
CLIENT="./client"
TARGET_DIR="logs"
BW_SUM_LOG='bw_sum.csv'
PROFILE=$1
TCP_FLOW_NUM=$2


let "ACTIVE_SUB1=$PROFILE&1"
let "ACTIVE_SUB2=$PROFILE&2"
let "ACTIVE_SUB3=$PROFILE&4"
let "YUVFILE=$PROFILE&8"
let "PROFILE=$PROFILE"

if [ "$YUVFILE" -eq 8 ]
then
BWMAX=500
BWMIN=150
else
BWMAX=1000
BWMIN=600
fi

VETH0_CMDS=$VETH0_CMDS" -x $BWMAX -n $BWMIN"
VETH2_CMDS=$VETH2_CMDS" -x $BWMAX -n $BWMIN"
VETH4_CMDS=$VETH4_CMDS" -x $BWMAX -n $BWMIN"

eval rm $TARGET_DIR"/*"

if [ "$ACTIVE_SUB1" -eq 1 ]
then
  sudo ip netns exec ns0 $VETH0_CMDS > $TARGET_DIR"/"veth0_bw.txt &
fi

if [ "$ACTIVE_SUB2" -eq 2 ]
then
  sudo ip netns exec ns0 $VETH2_CMDS > $TARGET_DIR"/"veth2_bw.txt &
fi

if [ "$ACTIVE_SUB3" -eq 4 ]
then
  sudo ip netns exec ns0 $VETH4_CMDS > $TARGET_DIR"/"veth4_bw.txt &
fi

sudo ip netns exec ns0 $SERVER "--profile="$PROFILE 2> $TARGET_DIR"/"server.log &
sudo ip netns exec ns1 $CLIENT "--profile="$PROFILE 2> $TARGET_DIR"/"client.log &

if [ "$TCP_FLOW_NUM" -gt 0 ]
then
  echo "iperf -s -p 45678" > iperf_s.sh
  chmod 777 iperf_s.sh
  echo "iperf -c 10.0.0.2 -p 45678 -t 900 -P $TCP_FLOW_NUM" > iperf_c.sh
  chmod 777 iperf_c.sh
  sudo ip netns exec ns1 ./iperf_s.sh &
  sudo ip netns exec ns0 ./iperf_c.sh &
fi

cleanup()
# example cleanup function
{
  pkill client
  pkill server
  ps -ef | grep 'run_veth0_const.sh' | grep -v grep | awk '{print $2}' | xargs kill
  ps -ef | grep 'run_veth0_cc.sh' | grep -v grep | awk '{print $2}' | xargs kill
  ps -ef | grep 'run_veth2_const.sh' | grep -v grep | awk '{print $2}' | xargs kill
  ps -ef | grep 'run_veth2_cc.sh' | grep -v grep | awk '{print $2}' | xargs kill
  ps -ef | grep 'run_veth4_const.sh' | grep -v grep | awk '{print $2}' | xargs kill
}
 
control_c()
# run if user hits control-c
{
  echo -en "\n*** Ouch! Exiting ***\n"
  cleanup
  exit $?
}

trap control_c SIGINT

#sudo ip netns exec ns0 ./run_veth0_const.sh > veth0_bw.txt &
sleep 1
for j in `seq 1 180`;
do
  for i in `seq 1 5`;
  do
    echo "0," >> $TARGET_DIR"/"$BW_SUM_LOG
    sleep 1
  done
  echo $j"*5 seconds"
  ./run_test_evaluator.sh $PROFILE
done
sleep 1

cleanup

DATE="Tests_"$(date +%y_%m_%d)
if [ ! -d "/home/balazs/Dropbox/MPRTP_Project/$DATE" ]; then
  mkdir "/home/balazs/Dropbox/MPRTP_Project/$DATE"
fi

TESTDIR="Profile_"$PROFILE"_"$(date +%H_%M_%S)
mkdir "/home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR"

if [ "$ACTIVE_SUB1" -eq 1 ]
then
  cp ../../Research-Scripts/gnuplot/mprtp-subflow-1-report.pdf /home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR/mprtp-subflow-1-report.pdf
fi

if [ "$ACTIVE_SUB2" -eq 2 ]
then
  cp ../../Research-Scripts/gnuplot/mprtp-subflow-2-report.pdf /home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR/mprtp-subflow-2-report.pdf
fi

if [ "$ACTIVE_SUB3" -eq 4 ]
then
  cp ../../Research-Scripts/gnuplot/mprtp-subflow-3-report.pdf /home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR/mprtp-subflow-3-report.pdf
fi

cp ../../Research-Scripts/gnuplot/mprtp-sum-report.pdf /home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR/mprtp-sum-report.pdf

mkdir "/home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR/mprtp_stat_files"
cp ../../Research-Scripts/gnuplot/mprtp_stat_files/* /home/balazs/Dropbox/MPRTP_Project/$DATE/$TESTDIR/mprtp_stat_files/




