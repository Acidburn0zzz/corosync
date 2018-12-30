### Welcome
The Corosync Cluster Engine is a Group Communication System with additional features for implementing high availability within applications. The project provides four C Application Programming Interface features:

* A closed process group communication model with virtual synchrony guarantees for creating replicated state machines.
* A simple availability manager that restarts the application process when it has failed.
* A configuration and statistics in-memory database that provide the ability to set, retrieve, and receive change notifications of information.
* A quorum system that notifies applications when quorum is achieved or lost.

Our project is used as a High Availability framework by projects such as Apache Qpid and Pacemaker.

We are always looking for developers or users interested in clustering or participating in our project.

The project is hosted by [GitHub](https://github.com/corosync).

### Corosync Downloads
Are provided by [GitHub download service](https://github.com/corosync/corosync/downloads).

### Support
Please see [Wiki](https://github.com/corosync/corosync/wiki) for more informations.

### Corosync Quality
Our project relies on a good mix of talented fresh engineering experience coupled experienced engineers to make great quality software. We validate our software using a test suite which runs on each commit of the tree.

Our [automated build and test infrastructure](http://www.corosync.org:8010) is provided by [buildbot](http://trac.buildbot.net/), [cbox](fabbione/cbox), Pacemaker's CTS, and our own Corosync specific CTS test cases.