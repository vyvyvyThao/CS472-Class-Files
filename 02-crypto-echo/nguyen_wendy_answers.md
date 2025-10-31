# Crypto Echo - Concept Questions Answers

**Name:** Wendy Nguyen  
**ID:** wn73    
**Assignment:** Crypto Echo TCP Client-Server Implementation

---

## Question 1: TCP vs UDP - Why Stateful Communication Matters

TCP is necessary for this application because it keeps track of the connection and remembers what happened before. When the client and server exchange encryption keys at the beginning, TCP holds onto that information for the entire conversation. This means both sides can trust they're using the same keys without having to check every time they send a message.

If we used UDP instead, several critical problems would arise. UDP treats each message independently, so we would need keep exchanging encryption keys with every message or find another way to track session state, both of which are inefficient and insecure. Also, UDP doesn't guarantee message delivery or order, so key exchange messages could be lost or arrive out of sequence, making the client and server unable to communicate.

## Question 2: Protocol Data Unit (PDU) Structure Design

Using a structured binary format with headers works better than simple text strings because it's clearer and more reliable. A text like "ENCRYPT:Hello World" would make it hard to tell where the command ends and the message begins, especially when dealing with encrypted data that might contain uncommon characters or symbols.

The structured approach with headers tells the receiver exactly what to expect - what type of message it is, which direction it's going, and how long the data part is. This removes any guessing and makes the system more reliable. It's also easier to add new types of messages later because we just need to define a new number for `msg_type` rather than redesigning how we parse everything.

## Question 3: The Payload Length Field

Even though TCP delivers everything reliably, we still need the `payload_len` field as TCP sends data as one continuous stream of bytes, not as separate messages. TCP doesn't know or care where one message ends and another begins - it just delivers bytes in order.

The `recv()` function may return partial messages or multiple messages in a single call. For example, if we send two PDUs quickly, they might arrive in one `recv()` call as a combined stream. Conversely, a large PDU might arrive across multiple `recv()` calls. The `payload_len` field tells us exactly how many bytes belong to each message, allowing us to properly reconstruct the individual PDUs from the TCP stream.

## Question 4: Key Exchange Protocol and Session State

Session-specific key generation is important for security and practical reasons. Pre-shared hardcoded keys would be vulnerable because they never change - if compromised, all communications would be at risk. Also, hardcoded keys would require secure distribution and coordination between all clients and servers.

When we generate fresh keys for each session, even if someone breaks into one conversation, all the other conversations stay secured. When the TCP connection ends, those keys become useless and cannot be used again. This connects the encryption directly to the network connection - when the connection dies, the encryption session dies with it. That's why TCP's ability to maintain state is so important - the network connection and the encryption session are tied together as one unit.

## Question 5: The Direction Field in the PDU Header

Including the `direction` field helps with debugging and makes the protocol clearer, even though we already know the client sends requests and the server sends responses. It clearly labels whether each message is a request or response, which makes troubleshooting much easier.

The field also serves as a safety check. If there's a bug where we accidentally process a request as a response (or vice versa), the direction field helps us catch that mistake. In addition, it makes the protocol more adaptable: if we later want both sides to send requests to each other, the direction field becomes necessary to distinguish between different message types.

---
