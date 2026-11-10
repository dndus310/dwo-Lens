# dwo-Lens 1.x entry point, kept so launchers written by dwo-Lens (__dwo-lens.bat in attached projects,
# old shortcuts) keep working. dwo-Forge runs from tools\forge.ps1; this file only forwards to it.
# The first command run through it migrates the project (__dwo-lens -> __dwo-forge, launcher, git exclude).
param(
    [Parameter(Mandatory = $true)][string]$Command,
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
$forge = Join-Path $PSScriptRoot 'forge.ps1'
if ($Rest) { & $forge -Command $Command -Project $Project @Rest } else { & $forge -Command $Command -Project $Project }
exit $LASTEXITCODE
