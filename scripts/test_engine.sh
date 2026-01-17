# Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
#
# This source code is licensed under the terms found in the
# LICENSE file in the root directory of this source tree.
#
# USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.

#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  Aurex Stock Exchange - Test Suite Builder${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# Check if build directory exists
if [ ! -d "build" ]; then
    echo -e "${YELLOW}⚠️  Build directory not found. Creating...${NC}"
    mkdir -p build
fi

cd build

# Configure CMake if needed
if [ ! -f "Makefile" ] && [ ! -f "build.ninja" ]; then
    echo -e "${BLUE}🔧 Configuring CMake...${NC}"
    cmake .. \
        -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake


    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ CMake configuration failed!${NC}"
        exit 1
    fi
fi

# Build test executable
echo -e "${BLUE}🔨 Building test suite...${NC}"
cmake --build . --target test_engine -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

if [ $? -ne 0 ]; then
    echo -e "${RED}❌ Test build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Build successful!${NC}"
echo ""

# Check if databases are running (optional check)
echo -e "${BLUE}🔍 Checking database availability...${NC}"
if command -v pg_isready &> /dev/null; then
    if pg_isready -h localhost -p 5432 &> /dev/null; then
        echo -e "${GREEN}✓ PostgreSQL is running${NC}"
    else
        echo -e "${YELLOW}⚠️  PostgreSQL not detected (tests will use in-memory mode)${NC}"
    fi
else
    echo -e "${YELLOW}⚠️  pg_isready not found (tests will use in-memory mode)${NC}"
fi

if command -v redis-cli &> /dev/null; then
    if redis-cli ping &> /dev/null; then
        echo -e "${GREEN}✓ Redis is running${NC}"
    else
        echo -e "${YELLOW}⚠️  Redis not detected (tests will skip Redis features)${NC}"
    fi
else
    echo -e "${YELLOW}⚠️  redis-cli not found (tests will skip Redis features)${NC}"
fi

echo ""
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  Running Tests${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# Run tests
./test_engine

TEST_RESULT=$?

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}  ✓ All tests passed!${NC}"
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
else
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${RED}  ✗ Some tests failed!${NC}"
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
fi

exit $TEST_RESULT
