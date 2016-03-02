#! /bin/bash
#Test3 - Playout test
#echo "Single congestion test on veth0"
#echo "S1:100ms - 2ms"
#To attach a TBF with a sustained maximum rate of 1mbit/s, 
#a peakrate of 2.0mbit/s, 
#a 10kilobyte buffer, with a pre-bucket queue size limit calculated so the TBF causes at most 70ms of latency, 
#with perfect peakrate behavior, enter:
#tc qdisc add dev veth0 root tbf rate 1mbit burst 10kb latency 70ms peakrate 2mbit minburst 1540
#tc qdisc change dev veth0 root tbf rate 1mbit burst 10kb latency 70ms peakrate 2mbit minburst 1540

LATENCY=100
JITTER=1
MAXBW=1000
MINBW=700

while [[ $# > 1 ]]
do
key="$1"

case $key in
    -x|--maxbw)
    MAXBW="$2"
    shift # past argument
    ;;
    -n|--minbw)
    MINBW="$2"
    shift # past argument
    ;;
    --default)
    DEFAULT=YES
    ;;
    *)
            # unknown option
    ;;
esac
shift # past argument or value
done

BW=$MAXBW
tc qdisc change dev veth0 parent 1:12 handle 2: pfifo limit 5
tc class change dev veth0 parent 1:1 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit 
tc qdisc change dev veth0 parent 1:12 handle 2: pfifo limit 5

for j in `seq 1 900`;
do
  echo $BW"000,"
  sleep 1
done

#tc qdisc change dev veth0 parent 1:12 netem delay 100ms 0ms
