#!/bin/bash
#
# Initialization script for VxCore development environment
# Run this script once after cloning the repository
#

set -e

echo "========================================"
echo "Initializing VxCore development environment"
echo "========================================"

# Get the script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Step 1: Initialize and update git submodules
echo ""
echo "[1/2] Initializing and updating git submodules..."
if ! git submodule update --init --recursive; then
    echo "Error: Failed to initialize submodules"
    exit 1
fi
echo "✓ Submodules initialized successfully"

# Step 2: Install pre-commit hook for main repository
echo ""
echo "[2/2] Installing pre-commit hook for main repository..."
HOOKS_DIR=".git/hooks"
if [ ! -d "$HOOKS_DIR" ]; then
    echo "Error: .git/hooks directory not found. Are you in a git repository?"
    exit 1
fi

if [ -f "$HOOKS_DIR/pre-commit" ]; then
    echo "Warning: pre-commit hook already exists. Creating backup..."
    cp "$HOOKS_DIR/pre-commit" "$HOOKS_DIR/pre-commit.backup"
fi

cp "scripts/pre-commit" "$HOOKS_DIR/pre-commit"
chmod +x "$HOOKS_DIR/pre-commit"
echo "✓ Main repository pre-commit hook installed"

# Done
echo ""
echo "========================================"
echo "✓ Initialization complete!"
echo "========================================"
echo ""
echo "Note: The pre-commit hooks require clang-format to be installed."
echo "If you don't have it installed, the hooks will skip formatting with a warning."
echo ""
