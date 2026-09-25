# High Availability (running more than one broker)

Several Coraine instances behind a load balancer, sharing one MongoDB.

## What an HA setup is

Three parts, and all three are needed:

1. **Two or more Coraine instances** (pods, containers, hosts).
2. **A load balancer** in front of them. A request may land on any instance - there is no session
   affinity and none is needed.
3. **MongoDB as a replica set**, shared by all instances, used through the `mongoc` database plugin.

## Why the instances must be kept in sync

Each instance keeps its subscriptions, registrations and @contexts in RAM, pre-compiled, so that
matching an update or forwarding a request never costs a database round trip. A subscription or
registration created on one instance lands in **that instance's** caches. Without
`--high-availability mongo` the other instances never learn about it, and because the load balancer
spreads requests, the symptoms look *intermittent*: notifications sent by some instances and not
others, forwarded requests that work on one instance and `404` on the next.

With `--high-availability mongo`, every instance watches the shared database with a MongoDB **change
stream** and is *pushed* every subscription, registration and @context change as it is written - no
polling, no interval to tune. Instances never connect to each other and need no knowledge of each
other: no peer list, no extra port.

## Requirements

| | |
|---|---|
| Database plugin | `mongoc` - an in-memory store is nobody else's, and the broker refuses `--high-availability mongo` with it. |
| MongoDB | A **replica set**. A single node is enough to run the mechanism (`mongod --replSet rs0`, then `rs.initiate()` once); real HA needs real redundancy in the database too. |
| MongoDB version | 4.0 or later (a change stream over the whole deployment). |
| MongoDB privileges | `find` and `changeStream` on **all** databases - see [below](#mongodb-privileges). |
| Coraine | The same version on every instance. |

The replica set, the credentials and the auth database are given in the connection URI:

```
--dbURI "mongodb://<user>:<password>@<host1>,<host2>,<host3>/?replicaSet=rs0&authSource=admin"
```

### MongoDB privileges

If MongoDB runs with authentication, the user Coraine connects as needs more than `readWrite` on
Coraine's own databases. Each instance watches the **whole deployment** with one change stream - a
tenant is a database of its own, and new tenants appear at any time - and MongoDB only allows that to
a user with the `find` and `changeStream` actions on every database. A role that grants exactly that:

```js
use admin
db.createRole({
  role:       "coraineHaWatch",
  privileges: [ { resource: { db: "", collection: "" }, actions: [ "find", "changeStream" ] } ],
  roles:      []
})
db.grantRolesToUser("<the Coraine user>", [ { role: "coraineHaWatch", db: "admin" } ])
```

(The built-in `readAnyDatabase` role also covers it, but grants more than is needed.)

## Enabling it

Off by default. Enable it on **every** instance, with the option or its environment variable - they
are the same setting:

```
coraine --dbURI "mongodb://.../?replicaSet=rs0" --high-availability mongo
CORAINE_HIGH_AVAILABILITY=mongo
```

An instance started with `--high-availability mongo` **refuses to start** - it does not fall back to
running unsynchronised - when:

* the database is not a replica set, or
* the change stream cannot be opened, typically because of the [privileges](#mongodb-privileges):

```
E: ... mongocHaWatchStart: --ha mongo: unable to open the change stream (not authorized on admin to execute command { aggregate: 1, pipeline: [ { $changeStream: { allChangesForCluster: true } } ] ... }). The stream watches the whole deployment ...
X: ... main: unable to start the HA channel 'mongo'
```

On Kubernetes this shows as pods in a crash loop with that message. Grant the role and they start.

## How it works

* One change stream per instance covers **every tenant** in a single thread. It is opened at startup,
  **before** the caches are loaded from the database, and nothing is applied until the load is done -
  so nothing written by another instance in between can be missed.
* Each event names the item and the operation - insert, update, delete; the instance re-reads that
  one item and updates the relevant cache. An instance also receives its own changes; applying them
  again is harmless.
* **Adding an instance** needs no action anywhere: it loads the caches at startup and then watches.
  Removing one needs nothing either.
* **If the connection drops** (a brief network loss, a primary failover), the MongoDB driver resumes the
  stream by itself. If it cannot, the error is logged and the stream is reopened 5 seconds later - from
  that moment, not from where it left off - and the instance keeps serving meanwhile:

  ```
  E: ... HA: change stream error (...) - restarting the stream in 5 seconds
  ```

  ⚠️ Changes made between the break and the reopen are **not** applied until the instance restarts.

### Consistency model

Propagation is **eventual, but push-based** - typically milliseconds after the write is committed. It
is *not* instantaneous: a client that creates a subscription and immediately sends an entity update
through the load balancer may have the update handled by an instance that has not applied the new
subscription yet.

## Troubleshooting: instances that disagree

The symptom: MongoDB holds everything, but each instance only knows what was created on it - a
subscription created through instance 1 is known to instance 1 only, one created through instance 2
to instance 2 only, and the rest know neither. Clients that list subscriptions to "reconcile" them
then see different answers depending on which instance replies, and create duplicates.

1. **Is `--high-availability mongo` set on every instance?** Without it there is no synchronisation at
   all, and this is exactly what that looks like.
2. **Is the change stream working?** Grep every instance's log for `HA: change stream error`. A
   `not authorized ... $changeStream ... allChangesForCluster` there means the
   [privileges](#mongodb-privileges) are missing. (Versions that refuse to start in that case show it
   as a failed start instead.)
3. **Grant the privileges**, then **restart the instances** - a rolling restart is fine. A running
   instance retries the stream every 5 seconds, so once the privileges are in place it starts receiving
   changes by itself, but changes made before that are **not** replayed. A restart makes every instance
   load the complete state from MongoDB and watch from there.
4. **Check**: create a subscription through the load balancer, then ask **each instance directly**
   (not through the load balancer) for `GET /ngsi-ld/v1/subscriptions/<id>`. Every instance must answer
   `200`.
5. **Clean up duplicates** created while the instances disagreed - synchronising does not remove them;
   they are real subscriptions. `GET /ngsi-ld/v1/subscriptions` answers from the instance's cache, so to
   see what is really stored, look in MongoDB (the `subscriptions` collection of the tenant's
   database), or ask each instance once it has been restarted, and `DELETE` the extras.

## What synchronisation does **not** cover

**Snapshots** (NGSI-LD § 5.16). Each instance loads them at startup, but changes to them are not
synchronised: a snapshot created through one instance is known to the others only after they restart.
