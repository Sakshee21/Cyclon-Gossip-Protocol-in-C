# Cyclon-Enhanced Gossip Protocol in C 

## Intro to Cyclon
Cyclon is a widely-adopted peer-sampling protocol designed to construct and
maintain random overlay networks in a fully decentralized and self-organizing
manner. Each node in the system maintains a small, fixed-size partial view of
other nodes, referred to as descriptors, which contain essential metadata such as
the node's identifier, network address, and a timestamp indicating the descriptor’s
creation time.

In each gossip cycle, a node selects the **oldest descriptor** in its view and initiates
a **gossip exchange** with the corresponding peer. During this exchange, both nodes
reciprocally send a subset of descriptors, including a fresh descriptor of
themselves, effectively replacing old entries with newer and randomly selected
ones. This process results in continuous reconfiguration of overlay links and
promotes randomness in peer connections.

Cyclon exhibits several desirable properties, including **Scalability**, through
bounded local state per node, **High reachability**, by promoting uniform
dissemination paths and **Robustness to node failures**, as randomization
mitigates the impact of individual departures.

Furthermore, the protocol maintains **bounded indegree** dynamics: while a node
increases its outdegree deterministically through self-insertion in each cycle, its
indegree is influenced by how frequently it is selected as a gossip partner. This
feedback loop leads to a stable equilibrium around the configured view size.

## Why Cyclon over Traditional Gossip Protocols

One of the significant advantages of Cyclon over traditional gossip protocol lies
in its ability to reduce message redundancy while preserving high reachability. In
traditional gossip, nodes often forward messages to multiple randomly selected
peers without awareness of the overall network state, leading to excessive
redundant transmissions especially as the number of nodes increases. Cyclon
addresses this inefficiency through its structured **peer-sampling mechanism**,
where each node maintains a fixed-size, constantly refreshed **partial view** of the
network. Instead of broadcasting messages to random peers, Cyclon nodes
engage in targeted, pairwise descriptor exchanges, effectively limiting the
number of messages disseminated in each cycle. This controlled and selective
interaction ensures that messages propagate efficiently across the network
without the uncontrolled flooding seen in classical gossip approaches. Moreover,
by refreshing the view based on **descriptor age**, Cyclon promotes diversity in peer
interactions, reducing the likelihood of repeatedly contacting the same nodes,
which is a major contributor to redundancy in traditional systems. As a result,
Cyclon achieves a more optimal balance between coverage and communication
overhead, minimizing redundancy without compromising on network
connectivity or robustness.

## More about the project

Well here there are a pre-defined set of 6 users which can be scaled up and down and according to the number of users that many number of terminals need to be opened in the system for simultaneous execution. The port number of each user is listed in the `users.txt` file and loopback ip that is `127.0.0.1` is used for the execution.

Upon execution, it can be clearly visualized how the **oldest descriptors get swapped** among the peers thereby changing the view of each peer. Gossip exchange happens with peers which weren’t initially in the view of a
node. This shows that swapping of the descriptors happen successfully.

In the middle of the simulation a **message** can be typed as well to see it get **communicated to all the peers** in their respective terminals demonstrating **efficient message forwarding** higlighting the efficiency of the **Cyclon enhanced gossip protocol**.


In each terminal:

```bash
gcc cyclon-gossip.c -o <executable_name>
./<executable_name> <port_number>

