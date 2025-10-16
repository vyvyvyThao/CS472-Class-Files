## Implementation Approach

I approached this assignment by working through the five function groups in order: time conversion, byte order handling, packet construction, protocol analysis, and output formatting. I started with the simpler conversion functions to build up to the more complex NTP algorithm implementation.

My main focus was getting the timestamps right since that's critical for NTP. I used `gettimeofday()` to capture microsecond-level precision and made sure to convert between the NTP epoch (1900) and Unix epoch (1970) using the provided constant. For the actual NTP calculations, I converted everything to doubles to avoid losing precision when doing the math.

## Challenges Faced

The biggest challenge was keeping track of network byte order conversions. I had to remember to use `htonl()` before sending the packet and `ntohl()` after receiving it. At first I was converting things multiple times by accident, but I eventually figured out to do it once right before/after network operations.

The reference_id field was also tricky because it means different things depending on the stratum value - sometimes it's an IP address, sometimes it's a 4-character code like "NIST", and sometimes it's just zero. I had to add different handling for each case and make sure the buffer was big enough.

Another issue was the fraction conversions between NTP's format (2^32 units) and regular microseconds. The math is straightforward but I had to be careful with the order of operations to avoid overflow and keep the precision.
