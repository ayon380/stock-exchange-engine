# Database Management for Stock Exchange Engine

This project uses PostgreSQL 18 and Redis databases that are configured to run **only when needed** (not automatically on system startup).

## Quick Commands

### macOS
```bash
# Check database status
./start_databases.sh status

# Start databases
./start_databases.sh start

# Stop databases  
./start_databases.sh stop

# Restart databases
./start_databases.sh restart

# Setup database (creates stockexchange DB and user)
./start_databases.sh setup
```

### Windows
```batch
# Check database status
start_databases.sh status

# Start databases
start_databases.sh start

# Stop databases
start_databases.sh stop

# Restart databases
start_databases.sh restart

# Setup database (creates stockexchange DB and user)
start_databases.sh setup
```

## Manual Database Operations

### PostgreSQL

#### macOS
```bash
# Start PostgreSQL manually
brew services start postgresql@18

# Stop PostgreSQL manually
brew services stop postgresql@18

# Connect to PostgreSQL
psql postgres

# Connect to stock exchange database
psql -h localhost -p 5432 -U myuser -d stockexchange
```

#### Windows
```batch
# Start PostgreSQL manually
net start postgresql-x64-18

# Stop PostgreSQL manually
net stop postgresql-x64-18

# Connect to PostgreSQL (assuming psql is in PATH)
psql postgres

# Connect to stock exchange database
psql -h localhost -p 5432 -U myuser -d stockexchange
```

### Redis

#### macOS
```bash
# Start Redis manually
brew services start redis

# Stop Redis manually
brew services stop redis

# Connect to Redis CLI
redis-cli
```

#### Windows
```batch
# Start Redis manually
net start redis

# Stop Redis manually
net stop redis

# Connect to Redis CLI
redis-cli
```

## Database Configuration

- **PostgreSQL 18**: 
  - Host: localhost
  - Port: 5432
  - Database: stockexchange
  - User: myuser
  - Password: mypassword

- **Redis**:
  - Host: localhost
  - Port: 6379
  - No authentication by default

## Typical Workflow

1. Start databases: `./start_databases.sh start` (macOS) or `start_databases.sh start` (Windows)
2. Run your application
3. Stop databases when done: `./start_databases.sh stop` (macOS) or `start_databases.sh stop` (Windows)

The databases will NOT start automatically when you boot your Mac/Windows machine or login.