# MariaDB KVT Storage Engine Deployment

This directory contains scripts and configuration files for deploying MariaDB with the KVT (Key-Value Transaction) storage engine backend.

## Files

- **my.cnf** - MariaDB configuration file with KVT-specific settings
- **deploy.sh** - Deployment script that builds and starts MariaDB with KVT
- **clean.sh** - Cleanup script that stops MariaDB and removes test files
- **README.md** - This documentation file

## Important Notes

⚠️ **KVT is currently in-memory only** - All data is lost when MariaDB stops. The test directory is cleaned on each deployment to ensure a fresh state.

## Usage

### Deploy MariaDB with KVT

```bash
./deploy.sh
```

This script will:
1. Check for existing MariaDB processes
2. Clean and create test directory at `/tmp/mariadb_kvt_test`
3. Build the KVT storage engine plugin
4. Initialize MariaDB data directory
5. Start MariaDB server on port 13306
6. Display connection information

### Connect to MariaDB

After deployment, connect using:
```bash
/home/lintaoz/work/mariadb/build_kvt/client/mariadb --socket=/tmp/mariadb_kvt_test/mysql.sock
```

Or with TCP:
```bash
/home/lintaoz/work/mariadb/build_kvt/client/mariadb -h 127.0.0.1 -P 13306
```

### Clean Up

```bash
./clean.sh
```

This script will:
1. Stop all MariaDB processes
2. Remove the entire test directory
3. Clean up socket files

## Configuration

The `my.cnf` file contains all MariaDB configuration settings. Key settings include:

- **Port**: 13306 (to avoid conflicts with system MariaDB)
- **Socket**: `/tmp/mariadb_kvt_test/mysql.sock`
- **Data directory**: `/tmp/mariadb_kvt_test/data`
- **Plugin directory**: Points to built KVT plugin
- **Plugin maturity**: Set to experimental (required for KVT)
- **Grant tables**: Disabled for easier testing

## Logs

- **Error log**: `/tmp/mariadb_kvt_test/mariadb.log`
- **General log**: `/tmp/mariadb_kvt_test/general.log`
- **Slow query log**: `/tmp/mariadb_kvt_test/slow.log`

## Modifying Configuration

To add or change configuration settings:

1. Edit `my.cnf` with your desired settings
2. Run `./clean.sh` to stop the current instance
3. Run `./deploy.sh` to start with new settings

## Troubleshooting

If deployment fails:
1. Check the error log at `/tmp/mariadb_kvt_test/mariadb.log`
2. Ensure the build directory exists at `/home/lintaoz/work/mariadb/build_kvt`
3. Verify KVT plugin compilation succeeded
4. Run `./clean.sh` and try again

## Development Notes

The KVT storage engine is compiled with `-D_GLIBCXX_DEBUG` to match MariaDB's debug build configuration. This is handled automatically by the deploy script.