# Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
#
# This source code is licensed under the terms found in the
# LICENSE file in the root directory of this source tree.
#
# USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.

#!/bin/bash

# Script to generate self-signed SSL certificates for the Stock Exchange Engine
# For PRODUCTION, replace with certificates from a trusted CA (Let's Encrypt, DigiCert, etc.)

echo "🔐 Generating SSL/TLS Certificates for Stock Exchange Engine"
echo "============================================================="
echo ""

# Certificate configuration
DAYS_VALID=365
KEY_SIZE=4096
COUNTRY="US"
STATE="California"
CITY="San Francisco"
ORG="Stock Exchange"
ORG_UNIT="Trading Platform"
COMMON_NAME="localhost"

# File paths
CERT_FILE="server.crt"
KEY_FILE="server.key"
CSR_FILE="server.csr"

echo "📝 Certificate Details:"
echo "   Country: $COUNTRY"
echo "   State: $STATE"
echo "   City: $CITY"
echo "   Organization: $ORG"
echo "   Common Name: $COMMON_NAME"
echo "   Valid for: $DAYS_VALID days"
echo "   Key size: $KEY_SIZE bits"
echo ""

# Check if certificates already exist
if [ -f "$CERT_FILE" ] && [ -f "$KEY_FILE" ]; then
    echo "⚠️  Certificates already exist!"
    read -p "   Do you want to overwrite them? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "❌ Certificate generation cancelled."
        exit 0
    fi
    echo ""
fi

# Generate private key
echo "🔑 Generating private key ($KEY_SIZE bit)..."
openssl genrsa -out "$KEY_FILE" $KEY_SIZE 2>/dev/null
if [ $? -ne 0 ]; then
    echo "❌ Failed to generate private key"
    exit 1
fi
echo "✅ Private key generated: $KEY_FILE"

# Set secure permissions on private key
chmod 600 "$KEY_FILE"
echo "🔒 Private key permissions set to 600 (owner read/write only)"

# Generate certificate signing request (CSR)
echo ""
echo "📄 Generating Certificate Signing Request..."
openssl req -new -key "$KEY_FILE" -out "$CSR_FILE" \
    -subj "/C=$COUNTRY/ST=$STATE/L=$CITY/O=$ORG/OU=$ORG_UNIT/CN=$COMMON_NAME" 2>/dev/null
if [ $? -ne 0 ]; then
    echo "❌ Failed to generate CSR"
    exit 1
fi
echo "✅ CSR generated: $CSR_FILE"

# Generate self-signed certificate
echo ""
echo "🎫 Generating self-signed certificate..."
openssl x509 -req -days $DAYS_VALID -in "$CSR_FILE" -signkey "$KEY_FILE" -out "$CERT_FILE" 2>/dev/null
if [ $? -ne 0 ]; then
    echo "❌ Failed to generate certificate"
    exit 1
fi
echo "✅ Certificate generated: $CERT_FILE"

# Clean up CSR file
rm -f "$CSR_FILE"

# Display certificate info
echo ""
echo "📋 Certificate Information:"
echo "============================================================="
openssl x509 -in "$CERT_FILE" -text -noout | grep -A 2 "Validity"
openssl x509 -in "$CERT_FILE" -text -noout | grep "Subject:"
openssl x509 -in "$CERT_FILE" -text -noout | grep "Public Key Algorithm"
echo "============================================================="
echo ""

# Display fingerprint
echo "🔍 Certificate Fingerprint (SHA256):"
openssl x509 -in "$CERT_FILE" -fingerprint -sha256 -noout
echo ""

echo "✅ SSL/TLS certificates generated successfully!"
echo ""
echo "📁 Files created:"
echo "   - $CERT_FILE (certificate)"
echo "   - $KEY_FILE (private key)"
echo ""
echo "⚠️  IMPORTANT NOTES:"
echo ""
echo "1. Self-signed certificates for DEVELOPMENT ONLY"
echo "   - Clients will see 'untrusted certificate' warnings"
echo "   - Not suitable for production use"
echo ""
echo "2. For PRODUCTION deployment:"
echo "   - Obtain certificates from a trusted Certificate Authority"
echo "   - Options: Let's Encrypt (free), DigiCert, Sectigo, etc."
echo "   - Use domain name instead of 'localhost'"
echo ""
echo "3. Security reminder:"
echo "   - NEVER commit $KEY_FILE to git (already in .gitignore)"
echo "   - Store private key securely"
echo "   - Rotate certificates before expiry"
echo ""
echo "4. Client connection:"
echo "   - Clients must verify certificate fingerprint"
echo "   - Or add certificate to trusted store"
echo ""
echo "🚀 Your stock exchange engine is now ready for encrypted connections!"
echo ""
