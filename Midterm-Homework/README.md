# Project Goal
Implement a client–server application in C with the following requirements:

- The server must use fork() to create a child process for handling each client connection (threads are not allowed).

- The server must be able to handle at least 10 clients concurrently, and a runtime proof must be provided.

- Provide a packet capture and analysis, using tcpdump for loopback traffic or Wireshark for communication between different computers.

- The program must support two levels of debug log control: one at compile time and one at runtime.

- Utility functions shared by both the client and server must be encapsulated into a dynamic shared library (.so) and linked by both sides.

- The client requests system information from the server as a one of your features design ( you can have many other features...)

- You must design and implement mechanisms for robustness to handle various abnormal or exceptional conditions.

- Provide at least three execution examples demonstrating the difference in program behavior with and without these robustness mechanisms implemented. 

- The project must be built using Makefile or CMake.
