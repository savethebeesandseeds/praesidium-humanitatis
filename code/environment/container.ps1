# SPDX-License-Identifier: MIT
param(
    [ValidateSet('plan', 'up', 'status', 'stop', 'shell', 'exec')]
    [string]$Action = 'status',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Command
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '../../tools/temporal-fusion-transformer/environment/container.ps1') -Action $Action -Command $Command
