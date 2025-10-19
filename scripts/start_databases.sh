#!/bin/bash

# Stock Exchange Engine - Database Management Script
# This script allows you to start/stop PostgreSQL 18 and Redis on demand
# Cross-platform support for macOS and Windows

# Detect OS
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "win32" ]]; then
    OS="windows"
else
    echo "Unsupported OS: $OSTYPE"
    exit 1
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

check_status() {
    echo -e "${YELLOW}Current database service status:${NC}"
    if [[ "$OS" == "macos" ]]; then
        brew services list | grep -E "(postgresql@18|redis)" | while read service status rest; do
            if [[ "$status" == "started" ]]; then
                echo -e "  ${GREEN}✓${NC} $service: $status"
            else
                echo -e "  ${RED}✗${NC} $service: $status"
            fi
        done
    elif [[ "$OS" == "windows" ]]; then
        # Check Windows services
        for service in "postgresql-x64-18" "redis"; do
            if sc query "$service" 2>/dev/null | grep -q "RUNNING"; then
                echo -e "  ${GREEN}✓${NC} $service: running"
            else
                echo -e "  ${RED}✗${NC} $service: stopped"
            fi
        done
    fi
    echo
}

start_databases() {
    echo -e "${GREEN}Starting PostgreSQL 18 and Redis...${NC}"
    if [[ "$OS" == "macos" ]]; then
        brew services start postgresql@18
        brew services start redis
    elif [[ "$OS" == "windows" ]]; then
        net start postgresql-x64-18 2>/dev/null || echo "Failed to start PostgreSQL service"
        net start redis 2>/dev/null || echo "Failed to start Redis service"
    fi
    echo
    check_status
}

stop_databases() {
    echo -e "${YELLOW}Stopping PostgreSQL 18 and Redis...${NC}"
    if [[ "$OS" == "macos" ]]; then
        brew services stop postgresql@18
        brew services stop redis
    elif [[ "$OS" == "windows" ]]; then
        net stop postgresql-x64-18 2>/dev/null || echo "Failed to stop PostgreSQL service"
        net stop redis 2>/dev/null || echo "Failed to stop Redis service"
    fi
    echo
    check_status
}

restart_databases() {
    echo -e "${YELLOW}Restarting PostgreSQL 18 and Redis...${NC}"
    if [[ "$OS" == "macos" ]]; then
        brew services restart postgresql@18
        brew services restart redis
    elif [[ "$OS" == "windows" ]]; then
        net stop postgresql-x64-18 2>/dev/null && net start postgresql-x64-18 2>/dev/null || echo "Failed to restart PostgreSQL service"
        net stop redis 2>/dev/null && net start redis 2>/dev/null || echo "Failed to restart Redis service"
    fi
    echo
    check_status
}

setup_database() {
    echo -e "${GREEN}Setting up stock exchange database...${NC}"
    
    if [[ "$OS" == "macos" ]]; then
        # Check if PostgreSQL is running
        if ! brew services list | grep "postgresql@18" | grep -q "started"; then
            echo "Starting PostgreSQL first..."
            brew services start postgresql@18
            sleep 3
        fi
    elif [[ "$OS" == "windows" ]]; then
        # Check if PostgreSQL service is running
        if ! sc query postgresql-x64-18 2>/dev/null | grep -q "RUNNING"; then
            echo "Starting PostgreSQL first..."
            net start postgresql-x64-18 2>/dev/null
            sleep 3
        fi
    fi
    
    # Create database and user
    echo "Creating database and user..."
    createdb stockexchange 2>/dev/null || echo "Database 'stockexchange' may already exist"
    psql postgres -c "CREATE USER myuser WITH PASSWORD 'mypassword';" 2>/dev/null || echo "User 'myuser' may already exist"
    psql postgres -c "GRANT ALL PRIVILEGES ON DATABASE stockexchange TO myuser;" 2>/dev/null || true
    psql postgres -c "ALTER USER myuser CREATEDB;" 2>/dev/null || true
    
    echo -e "${GREEN}Database setup complete!${NC}"
    echo "Connection string: host=localhost port=5432 dbname=stockexchange user=myuser password=mypassword"
}

case "$1" in
    start)
        start_databases
        ;;
    stop)
        stop_databases
        ;;
    restart)
        restart_databases
        ;;
    status)
        check_status
        ;;
    setup)
        setup_database
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status|setup}"
        echo
        echo "Commands:"
        echo "  start   - Start PostgreSQL 18 and Redis"
        echo "  stop    - Stop PostgreSQL 18 and Redis"
        echo "  restart - Restart PostgreSQL 18 and Redis"
        echo "  status  - Show current status of database services"
        echo "  setup   - Create the stock exchange database and user"
        echo
        check_status
        exit 1
        ;;
esac