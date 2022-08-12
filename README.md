# Proxy-Server

Intro to Computer Systems assignment :



### [proxy.c](proxy.c) contains my implementaion of proxy server that acts as an intermediary between clients making requests to access resources and the servers that satisfy those requests by serving content

There are three features in [proxy.c](proxy.c):


1. It accepts incoming connections, reads and parses requests, forwards requests to web servers, reads the servers’ responses, and forwards the responses to the corresponding clients
2. It deals with multiple concurrent connections in parallel
3. It uses a cache to keep recently used Web objects in memory dynamically to boost the performance of the proxy server

There are two significant classes of port numbers: HTTP request ports and the listening port specified by the command line argument 


1.   HTTP request port : an optional field in the URL of an HTTP request. For example: http://www.cmu.edu:8080/hub/index.html

> If no port is specified, the default HTTP port of 80 is used

> [proxy.c](proxy.c) handles and forward HTTP requests as HTTP/1.0 requests.

2.   Listening port: the port on which [proxy.c](proxy.c) listens for incoming connections. 



Usage: 

```
./proxy 12345
```



* In the case of invalid requests, or valid requests that [proxy.c](proxy.c) is unable to handle, it sends the appropriate HTTP status code back to the client.

* When reading and writing socket data, [proxy.c](proxy.c) uses the RIO package from the 
files `csapp.c` and `csapp.h`, which comprise the CS:APP package discussed in the CS:APP3e textbook

* When handling HTTP request from clients, [proxy.c](proxy.c) uses the functions in `http parser.h`, which defines the API for a small HTTP string parsing library. This library includes functions for extracting important data fields from HTTP response headers and storing them in a parser t struct.



