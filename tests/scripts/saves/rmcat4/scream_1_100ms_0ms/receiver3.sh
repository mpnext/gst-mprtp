./rcv_pipeline --sink=FAKESINK --codec=VP8 --stat=triggered_stat:temp/rcv_packets_3.csv:0 --plystat=triggered_stat:temp/ply_packets_3.csv:0 --receiver=RTP:5004 --playouter=SCREAM:RTP:10.0.0.1:5005  > temp/receiver_3.log