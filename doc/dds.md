# DDS in coraine

coraine speaks DDS (and so ROS 2) through its DDS bridge, built on eProsima's
DDS Enabler. Everything Orion-LD does with DDS, coraine does too - and in a few
places it deliberately does it differently. This page says what, in short.

## What you get

- **Topics, both ways.** A DDS sample becomes the value of an NGSI-LD
  attribute; writing that attribute publishes a sample. Which topic maps to
  which entity and attribute is set in the bridge configuration.
- **Services.** Writing a service's attribute calls the service. The reply
  lands in the same attribute, next to the request that asked for it. A write
  can wait a moment for the reply (`?ddsSync=true`), or answer at once and let
  the reply arrive later.
- **Actions.** Writing an action's attribute sends a goal. Each goal is its own
  instance of the attribute, and its feedback, status and result appear there
  as they come. A goal can carry an endpoint that is notified of its progress,
  and it can be cancelled.
- **Discovery.** Topics, services and actions that nobody configured are shown
  on one catch-all entity (`urn:ngsi-ld:dds:default`, as in Orion-LD), so the
  first question - *what is on this bus?* - answers itself.
- **Pre-population.** Every configured attribute exists from startup - its
  entity is created if it is not there yet - with the value `"uninitialized"`
  until the first sample.
- **History.** DDS updates go into temporal history like any other write.
- **The same configuration file.** A deployment moving from Orion-LD keeps its
  configuration, including `syncTimeoutMs`.

## Where it differs from Orion-LD

| | Orion-LD | coraine |
|---|---|---|
| A value that does not fit the topic's DDS type | stored, `204` - DDS never sees it | refused, `400` - nothing stored |
| Writing to DDS | the value is stored first, then published | **DDS first**: sent first, stored only if DDS took it |
| A service that is slow to answer | the write waits (5 s), then `504`, nothing stored | the write waits briefly if asked, then `202`; the reply lands when it comes |
| Discovered services | visible | visible **and callable** |
| After an action's last goal | the whole attribute is removed | the attribute stays |
| ROS names with a `/` | - | `robot1/navigate` becomes the attribute `robot1_navigate` |
| One write, several attributes, one refused by DDS | the whole write fails | the others are written, `207` names the refused one |

The idea behind all of them: **the entity mirrors what is on the wire.** A
value DDS never saw is not stored, and a slow robot does not make the broker
slow.

## How it is tested

Close to forty functional tests cover the DDS side, eight of them against real
ROS 2 nodes: talker and listener, a Fibonacci action server, service servers,
a `geometry_msgs/Twist` topic. They run on both of coraine's databases.

## Coming

- Using the time a write already spends forwarding to other brokers: whatever
  a service or a fast action answers in that time goes straight into the
  response.
- Creating and changing bridges and channels at runtime (today they come from
  the configuration file).
