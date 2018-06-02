# kproxy
Simple kqueue proxy

# Build
    clang -pthread -o kproxy kproxy.c

# Example
Redirect all TCP connections from 0.0.0.0:80 to 192.168.0.100:80, with the limit of 100 established connections and 2 worker threads:

    ./kproxy 192.168.0.100 80 100 2
