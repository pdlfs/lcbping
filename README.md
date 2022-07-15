# lcbping

The leveled call back ping program does a simple stop-and-wait
ping operation between two endpoints at various levels in the I/O
stack.  The intent is to have a tool that can measure operation
times (e.g. latency) at these levels.  Supported levels include
Mercury RPCs, the lower-level Mercury NA interface, and Intel/Qlogic
PSM IB.

For example, if you have Intel/Qlogic HCA cards on an Infiniband
network you can use lcbping to measure operation times directly
on top of the PSM API, using the Mercury NA API on top of the
PSM API, or using Mercury RPCs running on top of both the NA
and PSM APIs.  Ideally each layer you add on top of the base
PSM API should add minimal additional overhead.

# compiling

The lcbping program has a cmake-based build system (cmake 3.9
or newer required).  We assume you've already got or built
the support libs you want to use (e.g. Mercury, PSM).  The
build (including where to look for support libs) is configured
using cmake variables.  Variables can be set on the cmake
command line with '-D' (e.g. "cmake -DCMAKE_INSTALL_PREFIX=/usr/local").

CMake uses "prefix" directories to specify where to install
binaries and libraries (i.e. in the 'bin' and 'lib' subdirectories
of the prefix directory).   Prefix directories are also used
to search for support packages/libraries.

Configuration variables of interest include:

variable             | usage
-------------------- | --------------------
CMAKE_INSTALL_PREFIX | install prefix dir (also searched for support packages)
CMAKE_PREFIX_PATH    | additional prefix dirs to search for support packages
CMAKE_BUILD_TYPE     | Release, Debug, RelWithDebInfo (def=release)
LCB_MERCURY          | build mercury hg and na plugins (check, on, off)
LCB_PSM              | build PSM plugin (check, on, off)
LCB_HG_DLOG          | enabled dlog in hg plugin(def=off)
LCB_MPI              | enable MPI-based address exchange (def=off)

Variables set to 'check' mean that we check for a feature and
only enable/use it if it is found.  The 'check' value is normally
the default.

Example commands to checkout and build:
```
git clone https://github.com/pdlfs/lcbping
mkdir -p lcbping/build
cd lcbping/build
cmake -DLCB_MPI=ON -DLCB_MERCURY=ON -DCMAKE_INSTALL_PREFIX=/proj/lcb ..
make && make install
```
This assume that Mercury is already installed in either a standard
system directory (e.g. /usr) or in /proj/lcb.   Additional directories
can be searched by using the CMAKE_PREFIX_PATH list with items separated
by semicolons (e.g.  "-DCMAKE_PREFIX_PATH='/usr/a;/usr/b;/usr/c'").

# running

The command line usage is:
```
usage:
    client: lcbping type info count size addr-spec
    server: lcbping type info [addr-spec]
       MPI: mpirun lcbping type info count size MPI

  type=plugin type (hg, na, psm, etc.)
  info=additional plugin info
  count=# msgs to send
  size=message payload size in bytes
  addr-spec=on client: server address
                        or file with addr in it
  addr-spec=on server: filename to write srvr addr
```

The 'type' is the name of the plugin followed by a set of options
separated by a comma.  Possible values are:
* hg - use the Mercury RPC API
* hg,na_no_block - use Mercury RPCs with the NA_NO_BLOCK option turned on
* na - use the Mercury NA API
* na,na_no_block - use Mercury NA API with the NA_NO_BLOCK option turned on
* psm - use the PSM API

The 'info' is additional info for the plugin.  For Mercury-based APIs
this is the Mercury transport to use (e.g. bmi+tcp, ofi+tcp, ofi+verbs,
na_sm, psm+psm, etc.).  For PSM 'info' is currently not used (just set
it to 'psm').

The count is the number of ping messages/RPCs to send and the
size is the payload size.  The lcbping program current sends a
uint64_t sequence number in the payload, so the size must be
greater than or equal to sizeof(uint64_t).

When run in standalone server mode, the count and size may be
omitted.  The client will send the desired size to the server
in the first message/RPC and will send an EOF messages when 
it has sent "count" number of messages/RPCs.

When a lcbping client starts, it must have a way to get the
address of the lcbping server to talk to.   There are 3 ways
this can be done:
* the server address is specified as addr-spec on the command line
* the server writes its address to a file in a shared filesystem and the client reads it at startup.  In this case addr-spec is the filename of the shared file.
* the client and server run in a 2 proc MPI job and the server sends the client its address using MPI_Send() at startup.  To use this mode, we start the app with MPI and specify addr-spec as the string 'MPI' on the command line.


Examples:
```
# server address on the command line using PSM with 10 pings of 32 bytes.
# lcbping prints the server address at startup ("2260203" in the example).
# the user must note this and start the client with it.

./lcbping psm psm                   # server
./lcbping psm psm 10 32 2260203     # client

# server address stored in a file on a shared filesystem with BMI on NA.
./lcbping na bmi+tcp server-addr-file          # server
./lcbping na bmi+tcp 5 8 server-addr-file      # client

# using MPI address exchange (MPI must be enabled)
# this uses Mercury RPCs with na_no_block on ofi+tcp
mpirun -n 2 --hosts h0,h1 ./lcbping hg,na_no_block ofi+tcp 10 32 MPI
```

## hg dlog

The dlog is a debugging log option available in Mercury.  To use it
you must configure lcbping with the LCB_HG_DLOG variable set to ON.
Then when you run lcbping set the HGCB_DLOG_DUMP environment variable.
The program will generate dlog dumps in dlog_hg-*.log.   See the
cb_hg.c code for details.
