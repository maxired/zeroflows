Zero-Flows
==========

**Zero-Flows** is thin wrapper around *ZeroMQ*, *Jansson* and *Apache ZooKeeper*, helping to build versatile distributed workflows with a central configuration.

The workflows are chains of interconnected "steps", when each step is managed by several instances of the same service type.
It is designed to scale gracefully (each "step" can be scaled independently) and to give the administrator the maximum control on the deployment of the nodes.

This is not a competitor for Actors frameworks as Akka (even though it achieves the same goal), because it is much more raw and simplistic.


License
-------

**Zero-Flows** belongs to Jean-François SMIGIELSKI and all the contributors
to the project.

**Zero-Flows** is distributed under the terms of the GNU Affero GPL
(http://www.gnu.org/licenses/agpl.html).


Third-party libraries
---------------------

*ZeroMQ* 3.2.3 (http://zeromq.org) provides a taste a brokerless BUS.
The elementary transport unit is a ZeroMQ message, and **Zero-Flows**
lets you benefit of PUSH/PULL and PUB/SUB patterns of ZeroMQ.

*Apache ZooKeeper* 3.5 (http://zookeper.apache.org) is used to configure the
nodes and discover the peer nodes.

The configuration format used into *Zookeeper* is JSON, and we used *Jansson*
for the codec. It appears to be really easy to use.

**Zero-flows** has been built on top of the *Gnome Library*
(https://developer.gnome.org/glib/).  It provides easy-to-use data structures
and a lot of necessary features.


Installation
------------

You will require *cmake* to build and configure the Makefile.

     [nobody@localhost /tmp]$ cmake -DPREFIX=/tmp .
     [nobody@localhost /tmp]$ make install


TODO
----

This is still an early release.
There is a lot to do to make it easy to use.

