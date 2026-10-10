# Project 2 - Reliable Data Transfer
Name: Seth McDaniel
Email: sethmcdaniel437@u.boisestate.edu
Class: 452-001

## Experience
Following the advice of the professor, I got a cli ai setup for my dev environment. I started with reading through the assignment description and summarizing it in another document, sort of like a clearer project plan to make implementing it easier for the AI. I had it do the packet and protocol layer inside of lab.c but the I/O layer in main.c and overall it was "fine". I had a lot of little bugs, but largely the thing that fixed it the most was looking through the code and telling it to split functions apart so it had better layer seperation. Just because that made the tests much better at pinpointing what was going wrong. Splitting things apart fixed the big issue, the "just fix it" loop that I ran into where it would attempt to rewrite code to fix it, but it would just make a new problem. Im guessing its that the overall architecture was the culprit (vibe coded slop). So a more strict rewrite fixed it, mostly, that and some better test coverage. 

I think I learned the most about how this works from the results section of the project. Of just sort of playing with it to get an idea of how the protocal is supposed to work and why we care how the packets get there. In hindsight its simple, but I think I understand TCP and UDP more than I need to know at least. Keep having the result sections in your assignments where we need to answer questions when we are expceted to use AI because that was the most valuable part to me.

## Design
This design is split into three layers

### Packet Layer (top half of lab.c)
This block is responsible for cutting up the input (doesnt read the file thats I/O) to fit it into 10 byte packets and formats each packet for delivery. When packets arrive this is what also performs a checksum and rejects it if the checksum isnt correct

### Protocol Layer (bottom half of lab.c)
This is where we get that sender/receiver split. So the sender obviously sends the packets but it also keeps copies of the packets it hasnt gotten an acknowledge for yet (from that window) in case it needs to resend them. When it does get an acknowledge back, it moves over to the next window of packets to send. The receiver is the one that gets the files, but it only accepts them in the right order.

### I/O Layer (main.c)
This is the block of code that sets up the UDP sockets and parses the command line arguments. This is also where it reads and writes to files as mentioned in the packet layer section. 

### Why is it there?
Primary its there so its easier to test and ensure each piece works as expected, so when there is an issue its much easier to find (at least thats the most useful reason for me I found). One benefit of writing code in these more moduler pieces is that you can reuse code more efficiently.

## Results
| Window | Loss | Corrupt | Dup | Mean time (s) | Throughput (KiB/s) |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 0 | 0 | 103.560 | 9.888 |
| 16 | 0 | 0 | 0 | 6.67 | 153.523 |
| 1 | 0.05 | 0 | 0 | 131.084 | 7.812 |
| 16 | 0.05 | 0 | 0 | 22.104 | 46.326 |

From window 1 without loss, the expected time for 100ms per packet (which there should be 1025) is 102.5 seconds, the real run time was 103.560 though. Im pretty sure that most of the time (1.06s) comes from some processing overhead in the code for each packet sent/recieved.

Using that round trip time, we can mostly explain the speedup when we increase the window to 16, we get a time of 6.67s. This is not quite 16 times faster, its slightly slower (if it were 16 times faster by itself it would be 6.4725s). So there is about 200ms slower than expected. My theory is that the per window overhead increased, but since there are far less packets it speeds it up dramatically overall.

Comparing with the no loss time, the 5% loss affects the window 16 far more than the window 1 run time. I think while we do lose more windows in window 1, they are much quicker to resend since its just the 1 packet. So we lose a packet in window 16, we have to resend all of them again. So if 15 packets make it, but the last one is lost, the we gotta send all 16 again.