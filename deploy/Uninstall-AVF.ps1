#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Uninstalls the AVF (AV Filter) minifilter driver.

.DESCRIPTION
    This script will unload and remove the AVF driver completely.
#>

& "$PSScriptRoot\Install-AVF.ps1" -Uninstall
