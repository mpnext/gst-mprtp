./snd_pipeline --source=FILE:foreman_cif.yuv:1:352:288:2:25/1 --extradelay=50 --codec=VP8 --stat=triggered_stat:temp/snd_packets_2.csv:3 --sender=MPRTP:2:1:10.0.0.6:5004:2:10.0.1.6:5006 --scheduler=MPRTPFRACTAL:MPRTP:2:1:5005:2:5007 