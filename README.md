# Inter-process or cross-VM data exchange via CPU load modulation

## Principle

This PoC demonstrates a straightforward side-channel attack that allows one to construct reasonably robust
data links between multiple processes, possibly executed in different virtualized environments,
by modulating the CPU load or altering the state of any other shared hardware resource (such as CPU caches).

The method is based on CDMA modulation, which effectively allows one to pull useful signal from beneath the noise floor.
The sender and the receiver(s) share a specific CDMA spread code sequence.
Logic 1 is encoded by emitting the spread code in its original form; logic 0 is produced by inverting the code.
Each chip of the spread code is emitted by driving the state of the shared resource appropriately;
one trivial approach is to modulate the computing load on the CPU such that a high-level chip is emitted
by increasing the computing load and vice versa.

The receiver samples the state of the shared resource and feeds its observations into the CDMA correlator.
The correlator maintains an array of concurrent correlation channels;
each channel compares the received sample feed against the reference spread code (shared with the transmitter).
Each correlation channel has its copy of the spread code shifted by a fraction of the chip,
such that one of the channels is always guaranteed to match the sequence emitted by the transmitter,
while others would perceive the mismatching sequence as noise.

The correlator computes a weighted sum of the outputs of its channels,
where the weight of each channel is a function of the correlation between the received sample feed and the spread code.
The weighting ensures that uncorrelated channels are suppressed along with the noise in the medium.
This ensures that the data link is resilient against noise;
e.g., random variations of the processing load on the host generally do not cause link disruption.

The correlator also performs clock recovery in a similar manner
by computing a weighted sum of the code phase from each channel.

Any given system may host a theoretically unlimited number of such data links
provided that each link leverages sufficiently distinct spread code sequences.

The method provides reasonably robust VM-crossing data link at 1023 chips, 16 ms per chip,
resulting in the data rate of about 0.06 bits per second.
Data rates over 1 bit per second can be achieved if the data link does not cross
the boundaries of virtualized environments.
The speed vs. bit error rate trade-off can be adjusted by updating the chip period and the code length
defined in the header file.

## Demo

[![video](https://img.youtube.com/vi/PIUOHklFjrQ/maxresdefault.jpg)](https://youtu.be/PIUOHklFjrQ)

## Building

The build instructions are given at the top of each file.
