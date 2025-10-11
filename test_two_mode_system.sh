#!/bin/bash

# Two-Mode System Testing Script
# This script helps test the new TRADING/ADMIN mode functionality

set -e

echo "=========================================="
echo "  Stock Exchange Two-Mode System Test"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if database is running
echo -e "${YELLOW}[1/7] Checking PostgreSQL...${NC}"
if ! pg_isready -h localhost -p 5432 > /dev/null 2>&1; then
    echo -e "${RED}❌ PostgreSQL is not running!${NC}"
    echo "Please start PostgreSQL first:"
    echo "  ./start_databases.sh"
    exit 1
fi
echo -e "${GREEN}✅ PostgreSQL is running${NC}"
echo ""

# Check if Redis is running
echo -e "${YELLOW}[2/7] Checking Redis...${NC}"
if ! redis-cli ping > /dev/null 2>&1; then
    echo -e "${RED}❌ Redis is not running!${NC}"
    echo "Please start Redis first:"
    echo "  ./start_databases.sh"
    exit 1
fi
echo -e "${GREEN}✅ Redis is running${NC}"
echo ""

# Check if stock_engine binary exists
echo -e "${YELLOW}[3/7] Checking stock_engine binary...${NC}"
if [ ! -f "build/stock_engine" ]; then
    echo -e "${RED}❌ stock_engine binary not found!${NC}"
    echo "Please build the project first:"
    echo "  cd build && make -j4"
    exit 1
fi
echo -e "${GREEN}✅ stock_engine binary found${NC}"
echo ""

# Run database migration (optional, user choice)
echo -e "${YELLOW}[4/7] Database Schema Migration${NC}"
read -p "Do you want to run the database migration script? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Running migration..."
    psql -h localhost -p 5432 -U postgres -d stockexchange -f migrate_database_schema.sql
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ Migration completed successfully${NC}"
    else
        echo -e "${RED}❌ Migration failed${NC}"
        exit 1
    fi
else
    echo "Skipping migration"
fi
echo ""

# Create test accounts
echo -e "${YELLOW}[5/7] Creating test accounts...${NC}"
psql -h localhost -p 5432 -U postgres -d stockexchange <<EOF
-- Create test accounts if they don't exist
INSERT INTO user_accounts (user_id, cash, aapl_qty, googl_qty, msft_qty, amzn_qty, tsla_qty, 
                          buying_power, day_trading_buying_power, total_trades, realized_pnl, is_active)
VALUES 
    ('test_user_1', 10000000, 0, 0, 0, 0, 0, 10000000, 10000000, 0, 0, TRUE),
    ('test_user_2', 5000000, 10, 5, 8, 3, 2, 5000000, 5000000, 0, 0, TRUE),
    ('admin_user', 100000000, 0, 0, 0, 0, 0, 100000000, 100000000, 0, 0, TRUE)
ON CONFLICT (user_id) DO NOTHING;
EOF

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✅ Test accounts created${NC}"
    echo "   - test_user_1: \$100,000.00"
    echo "   - test_user_2: \$50,000.00 + stock positions"
    echo "   - admin_user: \$1,000,000.00"
else
    echo -e "${YELLOW}⚠️  Test accounts may already exist${NC}"
fi
echo ""

# Display test instructions
echo -e "${YELLOW}[6/7] Test Instructions${NC}"
echo "=========================================="
cat <<'INSTRUCTIONS'

The stock exchange will start in TRADING MODE.

🧪 Test Scenarios:

1. TRADING MODE (Default State):
   - Exchange is open, orders are being matched
   - You can submit buy/sell orders via gRPC/TCP
   - Balance changes are restricted (deposits only)

2. PRESS 'E' TO ENTER ADMIN MODE:
   - Exchange stops (all matching engines pause)
   - Admin menu appears
   - Test the following operations:
     a) View Account Balance (Option 1) - test_user_1
     b) Deposit Funds (Option 2) - add $10,000 to test_user_1
     c) View Balance again - verify deposit
     d) Withdraw Funds (Option 3) - remove $5,000 from test_user_1
     e) View Balance again - verify withdrawal
     f) Create New Account (Option 4) - create "new_test_user"
     g) View All Accounts (Option 5) - see all users

3. RESUME TRADING (Option 6):
   - Exchange restarts
   - Returns to TRADING MODE
   - Verify balances persisted correctly

4. DOUBLE-E TO EXIT:
   - Press 'E' again while in TRADING mode
   - Then press 'E' again within 3 seconds
   - System should shut down gracefully

5. VERIFY DATA PERSISTENCE:
   - After exit, restart stock_engine
   - Press 'E' to enter admin mode
   - View account balances
   - Verify all changes from previous session persisted

📋 Expected Results:
✅ Deposits increase cash and buying_power
✅ Withdrawals decrease cash and buying_power
✅ All changes are immediately saved to database
✅ Mode switching is smooth (no crashes)
✅ Data persists across restarts

🐛 Common Issues:
❌ "Database not connected" - check PostgreSQL
❌ "Account not found" - check user_id spelling
❌ "Insufficient funds" - check cash balance before withdrawal
❌ Build errors - ensure migration was run

INSTRUCTIONS
echo "=========================================="
echo ""

# Start the exchange
echo -e "${YELLOW}[7/7] Starting Stock Exchange...${NC}"
echo "Press Ctrl+C to stop (or use double-E from within the program)"
echo ""

cd build
./stock_engine

# After exit
echo ""
echo -e "${GREEN}=========================================="
echo "  Test Session Complete"
echo "==========================================${NC}"

# Ask if user wants to verify database state
echo ""
read -p "Do you want to view the final database state? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    psql -h localhost -p 5432 -U postgres -d stockexchange <<EOF
-- Display all accounts with formatted output
SELECT 
    user_id,
    cash / 100.0 AS cash_dollars,
    aapl_qty AS aapl_shares,
    googl_qty AS googl_shares,
    msft_qty AS msft_shares,
    amzn_qty AS amzn_shares,
    tsla_qty AS tsla_shares,
    buying_power / 100.0 AS buying_power_dollars,
    total_trades,
    realized_pnl / 100.0 AS realized_pnl_dollars,
    is_active
FROM user_accounts
ORDER BY cash DESC;

-- Summary statistics
SELECT 
    COUNT(*) AS total_accounts,
    COUNT(CASE WHEN is_active THEN 1 END) AS active_accounts,
    SUM(cash) / 100.0 AS total_cash_dollars,
    SUM(buying_power) / 100.0 AS total_buying_power_dollars,
    SUM(total_trades) AS total_trades_count,
    SUM(realized_pnl) / 100.0 AS total_realized_pnl_dollars
FROM user_accounts;
EOF
fi

echo ""
echo -e "${GREEN}✅ Two-Mode System Test Complete!${NC}"
echo ""
echo "📝 Next Steps:"
echo "   1. Review logs for any errors"
echo "   2. Verify database changes match your test operations"
echo "   3. Report any issues or unexpected behavior"
echo "   4. Test with concurrent clients (stress tests)"
echo ""
