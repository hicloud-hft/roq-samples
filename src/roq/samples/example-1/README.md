# Example 1

## Objective

* Demonstrate basic event processing

## What does it do?

* Define regex patterns used for subscribing market data
* Use the Trader interface to manage connections and event dispatching
* Use the asynchronous logger to print relevant incoming events

## Requirements

The example requires access to one or more market gateways running
on the same host.

## How to use

The example requires the `name` option specifying the login name used
when accessing the gateway(s).

Further arguments are UNIX domain socket end-points for the gateways.

Example:

```bash
./roq-samples-example-1 --name "example-1" \
    ~/deribit.sock \
    ~/coinbase-pro.sock
```

> The paths used for the UNIX domain sockets obviously depend on how
> you configure and launch your gateways.
