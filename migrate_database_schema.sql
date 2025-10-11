-- Database Migration Script for Two-Mode System
-- This script updates the user_accounts table schema to support the new field names

-- BACKUP YOUR DATABASE BEFORE RUNNING THIS SCRIPT!

BEGIN;

-- Step 1: Add new columns with temporary names
ALTER TABLE user_accounts 
ADD COLUMN IF NOT EXISTS aapl_qty BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS googl_qty BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS msft_qty BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS amzn_qty BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS tsla_qty BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS total_trades BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS realized_pnl BIGINT DEFAULT 0,
ADD COLUMN IF NOT EXISTS is_active BOOLEAN DEFAULT TRUE;

-- Step 2: Migrate data from old columns to new columns (if old columns exist)
DO $$
BEGIN
    -- Check if old columns exist and migrate data
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'aapl_position') THEN
        UPDATE user_accounts SET aapl_qty = aapl_position;
        UPDATE user_accounts SET googl_qty = COALESCE(goog_position, 0);
        UPDATE user_accounts SET msft_qty = COALESCE(msft_position, 0);
        UPDATE user_accounts SET amzn_qty = COALESCE(amzn_position, 0);
        UPDATE user_accounts SET tsla_qty = COALESCE(tsla_position, 0);
        UPDATE user_accounts SET total_trades = COALESCE(day_trades_count, 0);
        
        RAISE NOTICE 'Data migrated from old columns to new columns';
    ELSE
        RAISE NOTICE 'Old columns not found - this appears to be a fresh installation';
    END IF;
END $$;

-- Step 3: Drop old columns (if they exist)
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'goog_position') THEN
        ALTER TABLE user_accounts DROP COLUMN goog_position;
        RAISE NOTICE 'Dropped column: goog_position';
    END IF;
    
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'aapl_position') THEN
        ALTER TABLE user_accounts DROP COLUMN aapl_position;
        RAISE NOTICE 'Dropped column: aapl_position';
    END IF;
    
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'tsla_position') THEN
        ALTER TABLE user_accounts DROP COLUMN tsla_position;
        RAISE NOTICE 'Dropped column: tsla_position';
    END IF;
    
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'msft_position') THEN
        ALTER TABLE user_accounts DROP COLUMN msft_position;
        RAISE NOTICE 'Dropped column: msft_position';
    END IF;
    
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'amzn_position') THEN
        ALTER TABLE user_accounts DROP COLUMN amzn_position;
        RAISE NOTICE 'Dropped column: amzn_position';
    END IF;
    
    IF EXISTS (SELECT 1 FROM information_schema.columns 
               WHERE table_name = 'user_accounts' AND column_name = 'day_trades_count') THEN
        ALTER TABLE user_accounts DROP COLUMN day_trades_count;
        RAISE NOTICE 'Dropped column: day_trades_count';
    END IF;
END $$;

-- Step 4: Add indexes for performance
CREATE INDEX IF NOT EXISTS idx_user_accounts_active ON user_accounts(is_active) WHERE is_active = TRUE;
CREATE INDEX IF NOT EXISTS idx_user_accounts_cash ON user_accounts(cash);

-- Step 5: Verify the schema
DO $$
DECLARE
    col_count INTEGER;
BEGIN
    SELECT COUNT(*) INTO col_count
    FROM information_schema.columns
    WHERE table_name = 'user_accounts' 
    AND column_name IN ('aapl_qty', 'googl_qty', 'msft_qty', 'amzn_qty', 'tsla_qty', 
                        'total_trades', 'realized_pnl', 'is_active');
    
    IF col_count = 8 THEN
        RAISE NOTICE '✅ Migration successful! All new columns present.';
    ELSE
        RAISE EXCEPTION '❌ Migration verification failed! Expected 8 columns, found %', col_count;
    END IF;
END $$;

COMMIT;

-- Display final schema
SELECT 
    column_name, 
    data_type, 
    is_nullable,
    column_default
FROM information_schema.columns
WHERE table_name = 'user_accounts'
ORDER BY ordinal_position;

-- Display sample data to verify migration
SELECT 
    user_id,
    cash / 100.0 AS cash_dollars,
    aapl_qty,
    googl_qty,
    msft_qty,
    amzn_qty,
    tsla_qty,
    buying_power / 100.0 AS buying_power_dollars,
    total_trades,
    realized_pnl / 100.0 AS realized_pnl_dollars,
    is_active
FROM user_accounts
LIMIT 5;

-- Summary report
SELECT 
    COUNT(*) AS total_accounts,
    COUNT(CASE WHEN is_active THEN 1 END) AS active_accounts,
    SUM(cash) / 100.0 AS total_cash_dollars,
    SUM(buying_power) / 100.0 AS total_buying_power_dollars,
    SUM(total_trades) AS total_trades_count,
    SUM(realized_pnl) / 100.0 AS total_realized_pnl_dollars
FROM user_accounts;
