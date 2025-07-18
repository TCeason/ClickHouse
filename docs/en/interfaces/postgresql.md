---
description: 'Documentation for the PostgreSQL wire protocol interface in ClickHouse'
sidebar_label: 'PostgreSQL Interface'
sidebar_position: 20
slug: /interfaces/postgresql
title: 'PostgreSQL Interface'
---

# PostgreSQL Interface

ClickHouse supports the PostgreSQL wire protocol, which allows you to use Postgres clients to connect to ClickHouse. In a sense, ClickHouse can pretend to be a PostgreSQL instance - allowing you to connect a PostgreSQL client application to ClickHouse that is not already directly supported by ClickHouse (for example, Amazon Redshift).

To enable the PostgreSQL wire protocol, add the [postgresql_port](../operations/server-configuration-parameters/settings.md#postgresql_port) setting to your server's configuration file. For example, you could define the port in a new XML file in your `config.d` folder:

```xml
<clickhouse>
    <postgresql_port>9005</postgresql_port>
</clickhouse>
```

Startup your ClickHouse server and look for a log message similar to the following that mentions **Listening for PostgreSQL compatibility protocol**:

```response
{} <Information> Application: Listening for PostgreSQL compatibility protocol: 127.0.0.1:9005
```

## Connect psql to ClickHouse {#connect-psql-to-clickhouse}

The following command demonstrates how to connect the PostgreSQL client `psql` to ClickHouse:

```bash
psql -p [port] -h [hostname] -U [username] [database_name]
```

For example:

```bash
psql -p 9005 -h 127.0.0.1 -U alice default
```

:::note
The `psql` client requires a login with a password, so you will not be able connect using the `default` user with no password. Either assign a password to the `default` user, or login as a different user.
:::

The `psql` client prompts for the password:

```response
Password for user alice:
psql (14.2, server 22.3.1.1)
WARNING: psql major version 14, server major version 22.
         Some psql features might not work.
Type "help" for help.

default=>
```

And that's it! You now have a PostgreSQL client connected to ClickHouse, and all commands and queries are executed on ClickHouse.

:::note
The PostgreSQL protocol currently only supports plain-text passwords.
:::

## Using SSL {#using-ssl}

If you have SSL/TLS configured on your ClickHouse instance, then `postgresql_port` will use the same settings (the port is shared for both secure and insecure clients).

Each client has their own method of how to connect using SSL. The following command demonstrates how to pass in the certificates and key to securely connect `psql` to ClickHouse:

```bash
psql "port=9005 host=127.0.0.1 user=alice dbname=default sslcert=/path/to/certificate.pem sslkey=/path/to/key.pem sslrootcert=/path/to/rootcert.pem sslmode=verify-ca"
```

## Configuring ClickHouse user authentication with SCRAM-SHA-256 {#using-scram-sha256}

To ensure secure user authentication in ClickHouse, it is recommended to use the SCRAM-SHA-256 protocol. Configure the user by specifying the `password_scram_sha256_hex` element in the users.xml file. The password hash must be generated with num_iterations=4096.

Ensure that the psql client supports and negotiates SCRAM-SHA-256 during connection.

Example configuration for user `user_with_sha256` with the password `abacaba`:

```xml
<user_with_sha256>
    <password_scram_sha256_hex>04e7a70338d7af7bb6142fe7e19fef46d9b605f3e78b932a60e8200ef9154976</password_scram_sha256_hex>
</user_with_sha256>
```

View the [PostgreSQL docs](https://jdbc.postgresql.org/documentation/head/ssl-client.html) for more details on their SSL settings.
