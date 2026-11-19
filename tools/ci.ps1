# dwo-Forge local CI: a private Jenkins on this PC (127.0.0.1 only), installed from an offline bundle so it
# also works on closed networks, plus one pipeline job and git hooks per project.
# Windows PowerShell 5.1 compatible.
#
#   ci-bundle  [--out <dir>]                        (internet PC) download Jenkins LTS, a Java runtime and the plugins
#                                                    into dwo-forge-ci-bundle-<version>.zip
#   ci-install [--bundle <zip>] [--no-start]        install from the bundle (no network access)
#   ci-start | ci-stop | ci-status [--json]
#   ci-remove  [--keep-data|--delete-data]          stop, remove every project's job and hooks, delete the install
#   ci-job     [--remove]                           (project) create/update the project's job and git hooks
#   ci-trigger [--reason <text>] [--stages compile,lens,package] [--event <name>]   (project) queue a build
#   ci-hooks   [--remove]                       (project) only (re)install or remove the git hooks
#   ci-hook    <hook> <input file> <branch> <arg>   (project, called by the git hooks)
param(
    [Parameter(Mandatory = $true)][string]$Command,
    [string]$Project = '',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)

$ErrorActionPreference = 'Stop'
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$env:PYTHONUTF8 = '1'
$SystemDir = Split-Path -Parent $PSScriptRoot
$AppDataDir = Join-Path $env:LOCALAPPDATA 'dwo-Forge'
# DWO_FORGE_CI_HOME (user environment variable) moves the whole local CI, e.g. to a larger disk.
$CiDir = if ($env:DWO_FORGE_CI_HOME) { $env:DWO_FORGE_CI_HOME.TrimEnd('\') } else { Join-Path $AppDataDir 'ci' }
$Runtime = Join-Path $CiDir 'runtime'
$JenkinsHome = Join-Path $CiDir 'home'
$SecretsPath = Join-Path $CiDir 'secrets.json'
$CiStatePath = Join-Path $CiDir 'ci.json'
$ProjectsPath = Join-Path $CiDir 'projects.json'
$LogDir = Join-Path $CiDir 'logs'
$HookMark = 'dwo-forge-ci-hook'
$Hooks = @('post-commit', 'post-merge', 'pre-push', 'reference-transaction')
$Plugins = @('workflow-aggregator', 'junit')
if ($Project) { $Project = (Resolve-Path $Project).Path.TrimEnd('\') }

function Invoke-Utf8([scriptblock]$Block) {
    # git and Python (PYTHONUTF8) print UTF-8, but Windows PowerShell decodes captured native output with the
    # console code page (949 on Korean Windows), which garbles non-ASCII paths. Decode as UTF-8 for this call only.
    $prev = [Console]::OutputEncoding
    try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
    try { & $Block } finally { try { [Console]::OutputEncoding = $prev } catch { } }
}
function Say([string]$Message) { Write-Host "[dwo-forge] $Message" }
function Fail([string]$Message) { Write-Host "[dwo-forge] ERROR: $Message" -ForegroundColor Red; exit 1 }
function Has([string]$Flag) { return @($Rest) -contains $Flag }
function Opt([string]$Name, [string]$Default = '') {
    $a = @($Rest)
    for ($i = 0; $i -lt $a.Count - 1; $i++) { if ($a[$i] -eq $Name) { return $a[$i + 1] } }
    return $Default
}
function Read-JsonFile([string]$Path) {
    if (Test-Path $Path) { return Get-Content -Raw -Encoding UTF8 $Path | ConvertFrom-Json }
    return $null
}
function Write-JsonFile([string]$Path, $Object) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllText($Path, (ConvertTo-Json -InputObject $Object -Depth 8), $Utf8)
}

# ---------------------------------------------------------------- configuration
function Get-ConfigFiles {
    $files = @(Join-Path $SystemDir 'config\default.json')
    if ($Project) {
        $o = Join-Path $Project '__dwo-forge\config.json'
        if (Test-Path $o) { $files += $o }
    }
    return $files
}
function Get-CiConfig {
    $json = Invoke-Utf8 { & python (Join-Path $SystemDir 'tools\forge_ci.py') config --config @(Get-ConfigFiles) }
    return ($json | ConvertFrom-Json).ci
}
function Get-Port {
    $s = Read-JsonFile $CiStatePath
    if ($s -and $s.port) { return [int]$s.port }
    return [int](Get-CiConfig).port
}
function Get-BaseUrl { return "http://127.0.0.1:$(Get-Port)" }

# ---------------------------------------------------------------- Jenkins REST
function Get-AuthHeader {
    $s = Read-JsonFile $SecretsPath
    if (-not $s) { Fail 'Local CI is not installed. Run ci-install first.' }
    $pair = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($s.user):$($s.token)"))
    return @{ Authorization = "Basic $pair" }
}
function Invoke-Jenkins([string]$Method, [string]$Path, $Body = $null, [string]$ContentType = 'application/x-www-form-urlencoded', [int]$TimeoutSec = 30) {
    # Returns @{ Status; Content; Headers }. Never throws for HTTP errors (Status carries them); 0 = not reachable.
    $p = @{ Uri = (Get-BaseUrl) + $Path; Method = $Method; Headers = (Get-AuthHeader); UseBasicParsing = $true; TimeoutSec = $TimeoutSec }
    if ($Body -ne $null) { $p.Body = $Body; $p.ContentType = $ContentType }
    try {
        $r = Invoke-WebRequest @p -MaximumRedirection 0 -ErrorAction Stop
        return @{ Status = [int]$r.StatusCode; Content = $r.Content; Headers = $r.Headers }
    } catch [System.Net.WebException] {
        $resp = $_.Exception.Response
        if ($resp) { return @{ Status = [int]$resp.StatusCode; Content = ''; Headers = $resp.Headers } }
        return @{ Status = 0; Content = ''; Headers = @{} }
    } catch {
        # Windows PowerShell reports a 3xx under -MaximumRedirection 0 as this error (message is not about redirects).
        if ($_.FullyQualifiedErrorId -like 'MaximumRedirectExceeded*') { return @{ Status = 302; Content = ''; Headers = @{} } }
        return @{ Status = 0; Content = ''; Headers = @{} }
    }
}
function Test-Running {
    if (-not (Test-Path $SecretsPath)) { return $false }
    return (Invoke-Jenkins 'GET' '/api/json?tree=mode' -TimeoutSec 5).Status -eq 200
}
function Get-JobName {
    # A registered project keeps its job name.
    $mine = @(Get-Projects | Where-Object { $_.project -eq $Project -and $_.job })
    if ($mine.Count) { return $mine[0].job }
    $ci = Get-CiConfig
    $base = (Get-ChildItem $Project -Filter *.uproject -File | Select-Object -First 1).BaseName
    $ascii = ($base -replace '[^A-Za-z0-9_.-]+', '_').Trim('_')
    if (-not $ascii) { $ascii = 'project' }
    $name = "$($ci.jobPrefix)-$ascii"
    # Non-ASCII names (e.g. Korean) and a second checkout of the same project would otherwise share one job.
    $clash = @(Get-Projects | Where-Object { $_.job -eq $name -and $_.project -ne $Project }).Count -gt 0
    if ($clash -or $ascii -ne $base) {
        $sha = [Security.Cryptography.SHA1]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Project.ToLowerInvariant()))
        $name += '-' + (($sha[0..3] | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    return $name
}

# ---------------------------------------------------------------- bundle (internet PC)
function Save-Download([string]$Url, [string]$Dest) {
    Say "download $Url"
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing -Headers @{ 'User-Agent' = 'dwo-Forge-ci-bundle' }
}
function Get-Sha256([string]$Path) { return (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant() }
function Get-WarVersion([string]$War) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($War)
    try {
        $entry = $zip.GetEntry('META-INF/MANIFEST.MF')
        $reader = New-Object IO.StreamReader($entry.Open())
        $text = $reader.ReadToEnd(); $reader.Close()
        if ($text -match 'Jenkins-Version:\s*(\S+)') { return $Matches[1] }
        return 'unknown'
    } finally { $zip.Dispose() }
}

function Invoke-Bundle {
    $outDir = Opt '--out' (Join-Path $SystemDir 'bundles')
    $stage = Join-Path ([IO.Path]::GetTempPath()) ("dwo-forge-ci-bundle-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $stage, $outDir | Out-Null
    $dl = Join-Path $stage '_downloads'
    New-Item -ItemType Directory -Force $dl | Out-Null
    try {
        # Jenkins LTS
        $war = Join-Path $stage 'jenkins.war'
        Save-Download 'https://get.jenkins.io/war-stable/latest/jenkins.war' $war
        $shaFile = Join-Path $dl 'jenkins.war.sha256'
        Save-Download 'https://get.jenkins.io/war-stable/latest/jenkins.war.sha256' $shaFile
        $expected = ((Get-Content -Raw $shaFile).Trim() -split '\s+')[0].ToLowerInvariant()
        if ((Get-Sha256 $war) -ne $expected) { Fail 'jenkins.war checksum does not match get.jenkins.io' }
        $jenkinsVersion = Get-WarVersion $war

        # Java runtime (Eclipse Temurin 21 JRE)
        $assets = Invoke-RestMethod -UseBasicParsing 'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jre&os=windows&vendor=eclipse'
        $pkg = @($assets | Where-Object { $_.binary.package.name -like '*.zip' })[0]
        if (-not $pkg) { Fail 'No Temurin 21 JRE zip found at api.adoptium.net' }
        $jreZip = Join-Path $dl $pkg.binary.package.name
        Save-Download $pkg.binary.package.link $jreZip
        if ((Get-Sha256 $jreZip) -ne $pkg.binary.package.checksum.ToLowerInvariant()) { Fail 'JRE checksum does not match api.adoptium.net' }
        Expand-Archive $jreZip (Join-Path $dl 'jre') -Force
        $jreRoot = Get-ChildItem (Join-Path $dl 'jre') -Directory | Select-Object -First 1
        Move-Item $jreRoot.FullName (Join-Path $stage 'jre')
        $java = Join-Path $stage 'jre\bin\java.exe'

        # Plugins with their dependencies, resolved for this Jenkins version
        $rel = Invoke-RestMethod -UseBasicParsing -Headers @{ 'User-Agent' = 'dwo-Forge-ci-bundle' } 'https://api.github.com/repos/jenkinsci/plugin-installation-manager-tool/releases/latest'
        $asset = @($rel.assets | Where-Object { $_.name -match '^jenkins-plugin-manager-[\d.]+\.jar$' })[0]
        if (-not $asset) { Fail 'jenkins-plugin-manager jar not found in the latest release' }
        $pm = Join-Path $dl $asset.name
        Save-Download $asset.browser_download_url $pm
        $pluginDir = Join-Path $stage 'plugins'
        New-Item -ItemType Directory -Force $pluginDir | Out-Null
        Say "resolving plugins: $($Plugins -join ', ')"
        & $java -jar $pm --war $war --plugin-download-directory $pluginDir --plugins @Plugins
        if ($LASTEXITCODE -ne 0) { Fail 'plugin download failed' }
        $pluginCount = @(Get-ChildItem $pluginDir -File).Count

        Remove-Item -Recurse -Force $dl
        $info = @{ jenkinsVersion = $jenkinsVersion; java = $pkg.release_name; plugins = $Plugins; pluginFiles = $pluginCount
                   createdAt = (Get-Date).ToString('o') } | ConvertTo-Json -Compress
        $infoFile = Join-Path ([IO.Path]::GetTempPath()) 'dwo-forge-bundle-info.json'
        [IO.File]::WriteAllText($infoFile, $info, $Utf8)
        # Pass the JSON as a file: PowerShell 5.1 strips the double quotes from native command arguments.
        & python (Join-Path $SystemDir 'tools\forge_ci.py') make-manifest --dir $stage --info-file $infoFile
        $manifestExit = $LASTEXITCODE
        Remove-Item $infoFile -ErrorAction SilentlyContinue
        if ($manifestExit -ne 0) { Fail 'Could not write the bundle manifest.' }
        $zipPath = Join-Path $outDir "dwo-forge-ci-bundle-$jenkinsVersion.zip"
        if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
        Say "bundle: $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB)) MB, Jenkins $jenkinsVersion, $($pkg.release_name), $pluginCount plugin files)"
        Say 'Copy it to the closed-network PC and run: dwo-Forge.bat ci-install --bundle <zip>'
    } finally {
        Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- install / start / stop
function Find-Bundle {
    $b = Opt '--bundle'
    if ($b) {
        if (-not (Test-Path $b)) { Fail "Bundle not found: $b" }
        return (Resolve-Path $b).Path
    }
    $found = @(Get-ChildItem (Join-Path $SystemDir 'bundles') -Filter 'dwo-forge-ci-bundle-*.zip' -File -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending)
    if ($found.Count) { return $found[0].FullName }
    Fail ("No CI bundle. Put dwo-forge-ci-bundle-<version>.zip in $(Join-Path $SystemDir 'bundles') or pass --bundle <zip>. " +
          'Make one on a PC with internet: dwo-Forge.bat ci-bundle')
}

function New-Secret([int]$Bytes) {
    $b = New-Object byte[] $Bytes
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
    return (($b | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Write-InitScript {
    # Runs at every Jenkins start: local account + fixed API token for dwo-Forge, no setup wizard, logged-in
    # users only, one executor, no usage statistics, and this URL (127.0.0.1 only).
    $dir = Join-Path $JenkinsHome 'init.groovy.d'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $groovy = @'
import jenkins.model.*
import hudson.security.*
import jenkins.security.ApiTokenProperty
import jenkins.install.InstallState
def secretsPath = System.getProperty('dwo.forge.secrets')
def s = new groovy.json.JsonSlurper().parse(new File(secretsPath))
def j = Jenkins.get()
def realm = (j.securityRealm instanceof HudsonPrivateSecurityRealm) ? j.securityRealm : new HudsonPrivateSecurityRealm(false)
if (realm.getUser(s.user) == null) { realm.createAccount(s.user, s.password) }
j.setSecurityRealm(realm)
def strategy = new FullControlOnceLoggedInAuthorizationStrategy()
strategy.setAllowAnonymousRead(false)
j.setAuthorizationStrategy(strategy)
def u = hudson.model.User.getById(s.user, false)
def tokens = u.getProperty(ApiTokenProperty.class).tokenStore
if (!tokens.tokenListSortedByName.any { it.name == 'dwo-forge' }) {
    // Jenkins 2.5xx added an expiration date (null = never expires); older releases take two arguments.
    if (tokens.metaClass.respondsTo(tokens, 'addFixedNewToken', String, String, java.time.LocalDate)) {
        tokens.addFixedNewToken('dwo-forge', s.token, (java.time.LocalDate) null)
    } else {
        tokens.addFixedNewToken('dwo-forge', s.token)
    }
}
u.save()
j.setNumExecutors(1)
j.setSlaveAgentPort(-1)
j.setNoUsageStatistics(true)
// Offline: plugins come from the bundle, so there is no update site to contact (tool installers included).
if (j.updateCenter.sites.size() > 0) { j.updateCenter.sites.replaceBy([]); j.updateCenter.save() }
j.setInstallState(InstallState.INITIAL_SETUP_COMPLETED)
JenkinsLocationConfiguration.get().setUrl("http://127.0.0.1:${s.port}/")
j.save()
'@
    [IO.File]::WriteAllText((Join-Path $dir 'dwo-forge.groovy'), $groovy, $Utf8)
}

function Invoke-Install {
    $bundle = Find-Bundle
    # Always stop: a Jenkins that runs but rejects our account still locks the runtime files.
    Stop-Jenkins
    $stage = Join-Path $CiDir '_staging'
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $stage | Out-Null
    Say "extracting $bundle"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($bundle, $stage)
    $check = Invoke-Utf8 { & python (Join-Path $SystemDir 'tools\forge_ci.py') verify-bundle --dir $stage }
    if ($LASTEXITCODE -ne 0) { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue; Write-Host $check; Fail 'The bundle is damaged or incomplete.' }
    $manifest = $check | ConvertFrom-Json
    Say "bundle ok: Jenkins $($manifest.jenkinsVersion), $($manifest.java), $($manifest.fileCount) files verified"

    Remove-Item -Recurse -Force $Runtime -ErrorAction SilentlyContinue
    if (Test-Path $Runtime) {
        Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
        Fail "Could not replace $Runtime (files in use). Close programs using it (java.exe) and run ci-install again."
    }
    Move-Item $stage $Runtime
    $pluginHome = Join-Path $JenkinsHome 'plugins'
    New-Item -ItemType Directory -Force $pluginHome, $LogDir | Out-Null
    # Replace bundled plugins (and their unpacked folders) so an upgrade never mixes versions.
    foreach ($f in Get-ChildItem (Join-Path $Runtime 'plugins') -File) {
        $base = [IO.Path]::GetFileNameWithoutExtension($f.Name)
        Remove-Item -Recurse -Force (Join-Path $pluginHome $base) -ErrorAction SilentlyContinue
        Get-ChildItem $pluginHome -File -Filter "$base.*" -ErrorAction SilentlyContinue | Remove-Item -Force
        Copy-Item $f.FullName (Join-Path $pluginHome ($base + '.jpi'))
    }
    if (-not (Test-Path $SecretsPath)) {
        $port = [int](Get-CiConfig).port
        Write-JsonFile $SecretsPath ([ordered]@{ user = 'dwo-forge'; password = (New-Secret 16); token = '11' + (New-Secret 16); port = $port })
        Say "account: dwo-forge (password and API token in $SecretsPath)"
    }
    $secrets = Read-JsonFile $SecretsPath
    $wantPort = Opt '--port'
    if ($wantPort) {
        if ($wantPort -notmatch '^\d{4,5}$' -or [int]$wantPort -lt 1024 -or [int]$wantPort -gt 65535) { Fail '--port takes a number from 1024 to 65535.' }
        $secrets.port = [int]$wantPort
        Write-JsonFile $SecretsPath $secrets
        Say "port: $wantPort"
    }
    Write-InitScript
    Write-JsonFile $CiStatePath ([ordered]@{ port = [int]$secrets.port; jenkinsVersion = $manifest.jenkinsVersion; java = $manifest.java
                                             bundle = (Split-Path -Leaf $bundle); installedAt = (Get-Date).ToString('o') })
    Say "installed: $CiDir"
    if (-not (Has '--no-start')) { Start-Jenkins }
}

function Get-JenkinsProcess {
    $s = Read-JsonFile (Join-Path $CiDir 'process.json')
    if (-not $s) { return $null }
    $p = Get-CimInstance Win32_Process -Filter "ProcessId = $($s.pid)" -ErrorAction SilentlyContinue
    if ($p -and $p.Name -eq 'java.exe') { return $p }
    return $null
}

function Start-Jenkins {
    if (-not (Test-Path (Join-Path $Runtime 'jenkins.war'))) { Fail 'Local CI is not installed. Run ci-install first.' }
    if (Test-Running) { Say "already running: $(Get-BaseUrl)/"; return }
    $port = Get-Port
    $busy = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
    if ($busy) { Fail "Port $port is used by another program (pid $($busy[0].OwningProcess)). Move the local CI to a free port: dwo-Forge.bat ci-install --port 8099" }
    New-Item -ItemType Directory -Force $LogDir | Out-Null
    $java = Join-Path $Runtime 'jre\bin\java.exe'
    $log = Join-Path $LogDir 'jenkins.log'
    if (Test-Path $log) { Move-Item -Force $log "$log.1" }
    $javaArgs = "-Xmx1g -Djava.awt.headless=true -Dfile.encoding=UTF-8 -Djenkins.install.runSetupWizard=false " +
                "-Dhudson.model.DownloadService.never=true -Dhudson.model.UpdateCenter.never=true " +
                # No inbound agent port: it listens on every interface (firewall prompt, exposed port) and a local CI
                # has no remote agents.
                "-Djenkins.model.Jenkins.slaveAgentPort=-1 -Djenkins.model.Jenkins.slaveAgentPortEnforce=true " +
                "-DJENKINS_HOME=`"$JenkinsHome`" -Ddwo.forge.secrets=`"$SecretsPath`" " +
                "-jar `"$(Join-Path $Runtime 'jenkins.war')`" --httpListenAddress=127.0.0.1 --httpPort=$port --webroot=`"$(Join-Path $CiDir 'war')`""
    # Started through WMI so it is not part of the caller's process tree: closing the dwo-Forge window or a
    # git hook finishing must not stop Jenkins.
    $startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
    $cmd = "cmd.exe /d /c `"`"$java`" $javaArgs > `"$log`" 2>&1`""
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $CiDir; ProcessStartupInformation = $startup }
    if ($r.ReturnValue -ne 0) { Fail "Could not start Jenkins (Win32_Process.Create returned $($r.ReturnValue))" }
    Say "starting Jenkins on $(Get-BaseUrl)/ ..."
    $deadline = (Get-Date).AddSeconds(240)
    $javaPid = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (-not $javaPid) {
            $j = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($r.ProcessId)" -ErrorAction SilentlyContinue | Where-Object Name -eq 'java.exe' | Select-Object -First 1
            if ($j) { $javaPid = $j.ProcessId; Write-JsonFile (Join-Path $CiDir 'process.json') @{ pid = $javaPid; startedAt = (Get-Date).ToString('o') } }
        }
        if (Test-Running) { Say "running: $(Get-BaseUrl)/"; return }
        if ($javaPid -and -not (Get-CimInstance Win32_Process -Filter "ProcessId = $javaPid" -ErrorAction SilentlyContinue)) { break }
    }
    Get-Content $log -Tail 30 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    # Do not leave a half-started Jenkins holding the port.
    if ($javaPid) { & taskkill /PID $javaPid /T /F 2>$null | Out-Null }
    Remove-Item (Join-Path $CiDir 'process.json') -ErrorAction SilentlyContinue
    Fail "Jenkins did not come up. Log: $log"
}

function Stop-Jenkins {
    $graceful = Test-Running
    if ($graceful) {
        Say 'stopping Jenkins ...'
        [void](Invoke-Jenkins 'POST' '/exit' -TimeoutSec 10)
    }
    $deadline = (Get-Date).AddSeconds($(if ($graceful) { 60 } else { 0 }))
    while ((Get-Date) -lt $deadline -and (Get-JenkinsProcess)) { Start-Sleep -Seconds 1 }
    $p = Get-JenkinsProcess
    if ($p) { & taskkill /PID $p.ProcessId /T /F 2>$null | Out-Null }
    Remove-Item (Join-Path $CiDir 'process.json') -ErrorAction SilentlyContinue
    Say 'stopped'
}

function Get-Projects {
    $p = Read-JsonFile $ProjectsPath
    if (-not $p) { return @() }
    return @($p)
}
function Save-Projects($List) { Write-JsonFile $ProjectsPath @($List) }

function Invoke-Status {
    $installed = Test-Path (Join-Path $Runtime 'jenkins.war')
    $state = Read-JsonFile $CiStatePath
    $running = $installed -and (Test-Running)
    $doc = [ordered]@{
        installed = $installed; running = $running; url = "$(Get-BaseUrl)/"; port = (Get-Port)
        jenkinsVersion = $(if ($state) { $state.jenkinsVersion } else { $null }); java = $(if ($state) { $state.java } else { $null })
        dir = $CiDir; secrets = $SecretsPath; projects = @(Get-Projects)
        bundles = @(Get-ChildItem (Join-Path $SystemDir 'bundles') -Filter 'dwo-forge-ci-bundle-*.zip' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    }
    if ($Project) {
        $doc.job = Get-JobName
        $pc = Read-JsonFile (Join-Path $Project '__dwo-forge\ci.json')
        $doc.registered = [bool]$pc
        $doc.hooks = @(if ($pc) { $pc.hooks })
    }
    if (Has '--json') { ConvertTo-Json -InputObject $doc -Depth 6; return }
    if (-not $installed) { Say 'local CI: not installed (ci-install)'; return }
    Say "local CI: Jenkins $($doc.jenkinsVersion) $(if ($running) { "running at $($doc.url)" } else { 'stopped (ci-start)' })"
    Say "account: dwo-forge (password: $SecretsPath)"
    foreach ($p in $doc.projects) { Say "  job $($p.job) <- $($p.project)" }
}

function Invoke-Remove {
    $keep = Has '--keep-data'
    if (-not $keep -and -not (Has '--delete-data')) {
        if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
            $a = Read-Host 'Keep the build history (jobs) outside the install? [Y/n]'
            $keep = -not ($a -match '^[nN]')
        } else { $keep = $true }
    }
    foreach ($p in Get-Projects) {
        if (Test-Path $p.project) {
            Remove-ProjectHooks $p.project
            Remove-Item (Join-Path $p.project '__dwo-forge\ci.json') -ErrorAction SilentlyContinue
            Remove-Item -Recurse -Force (Join-Path $p.project '__dwo-forge\ci') -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path $CiDir) { Stop-Jenkins }
    if ($keep -and (Test-Path (Join-Path $JenkinsHome 'jobs'))) {
        $dest = Join-Path $AppDataDir ("uninstalled\ci-" + (Get-Date).ToString('yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force $dest | Out-Null
        Move-Item (Join-Path $JenkinsHome 'jobs') $dest
        Say "build history kept at $dest"
    }
    Remove-Item -Recurse -Force $CiDir -ErrorAction SilentlyContinue
    if (Test-Path $CiDir) { Fail "Some files could not be removed: $CiDir (is Jenkins still running?)" }
    Say 'local CI removed (Jenkins, its data, every project job and git hook).'
}

# ---------------------------------------------------------------- project: git hooks
function Get-HooksDir([string]$Proj) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $null }
    $ErrorActionPreference = 'Continue'
    $d = Invoke-Utf8 { & git -C $Proj rev-parse --git-path hooks 2>$null }
    if ($LASTEXITCODE -ne 0 -or -not $d) { return $null }
    if (-not [IO.Path]::IsPathRooted($d)) { $d = Join-Path $Proj $d }
    return [IO.Path]::GetFullPath($d)
}

function Get-HookScript([string]$Name, [string]$Proj) {
    $ps1 = (Join-Path $SystemDir 'tools\ci.ps1') -replace '\\', '/'
    $proj = $Proj -replace '\\', '/'
    $state = (Join-Path $Proj '__dwo-forge\ci') -replace '\\', '/'
    $stdin = if ($Name -in @('pre-push', 'reference-transaction')) { 1 } else { 0 }
    return @"
#!/bin/sh
# $HookMark`: dwo-Forge local CI. Removed by '__dwo-forge.bat ci-job --remove', ci-remove or uninstall.
# The hook this replaced (if any) is kept next to it as $Name.dwo-forge-orig and runs first.
hook=$Name
dir=`$(dirname "`$0")
input=
if [ $stdin = 1 ]; then input=`$(cat); fi
if [ -x "`$dir/`$hook.dwo-forge-orig" ]; then
  if [ $stdin = 1 ]; then printf '%s\n' "`$input" | "`$dir/`$hook.dwo-forge-orig" "`$@" || exit `$?
  else "`$dir/`$hook.dwo-forge-orig" "`$@" || exit `$?; fi
fi
case "`$hook" in
  pre-push)
    # Synchronous: the remote-tracking ref moves right after the push, before a background job could start.
    mkdir -p "$state" && { echo "`$1 `$(date +%s)"; printf '%s\n' "`$input"; } > "$state/push-pending.raw"
    exit 0 ;;
  reference-transaction)
    [ "`$1" = committed ] || exit 0
    printf '%s\n' "`$input" | grep -qE '(^0{40} [0-9a-f]+ refs/heads/| refs/remotes/)' || exit 0 ;;
esac
mkdir -p "$state"
tmp="$state/hook-`$`$-`$(date +%s).txt"
printf '%s\n' "`$input" > "`$tmp"
branch=`$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "$ps1" -Command ci-hook -Project "$proj" "`$hook" "`$tmp" "`$branch" "`$1" </dev/null >/dev/null 2>&1 &
exit 0
"@
}

function Install-ProjectHooks([string]$Proj) {
    $dir = Get-HooksDir $Proj
    if (-not $dir) { Say 'git hooks: not a git repository, skipped (use ci-trigger or HTTP to start builds)'; return @() }
    $top = Invoke-Utf8 { & git -C $Proj rev-parse --show-toplevel 2>$null }
    if ($top -and ([IO.Path]::GetFullPath($top) -ne [IO.Path]::GetFullPath($Proj))) {
        Say "git hooks: note - the repository root is $top, so commits anywhere in it trigger this project's builds."
    }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $installed = @()
    foreach ($h in $Hooks) {
        $path = Join-Path $dir $h
        if ((Test-Path $path) -and -not ((Get-Content -Raw $path) -match $HookMark)) {
            Move-Item -Force $path "$path.dwo-forge-orig"
            Say "git hooks: kept the existing $h hook (runs first)"
        }
        [IO.File]::WriteAllText($path, ((Get-HookScript $h $Proj) -replace "`r`n", "`n"), $Utf8)
        $installed += $h
    }
    Say "git hooks: $($installed -join ', ') in $dir"
    return $installed
}

function Remove-ProjectHooks([string]$Proj) {
    $dir = Get-HooksDir $Proj
    if (-not $dir -or -not (Test-Path $dir)) { return }
    foreach ($h in $Hooks) {
        $path = Join-Path $dir $h
        if ((Test-Path $path) -and ((Get-Content -Raw $path) -match $HookMark)) {
            Remove-Item -Force $path
            if (Test-Path "$path.dwo-forge-orig") { Move-Item -Force "$path.dwo-forge-orig" $path }
        }
    }
    Remove-Item -Recurse -Force (Join-Path $Proj '__dwo-forge\ci') -ErrorAction SilentlyContinue
    Say "git hooks removed from $dir"
}

# ---------------------------------------------------------------- project: job, trigger, hook events
function Require-Project {
    if (-not $Project -or -not (Test-Path (Join-Path $Project '__dwo-forge\state.json'))) { Fail 'Run this in a project connected to dwo-Forge (__dwo-forge.bat ci-job).' }
}

function Ensure-Running {
    if (-not (Test-Path (Join-Path $Runtime 'jenkins.war'))) { Fail 'Local CI is not installed. Run ci-install first.' }
    if (-not (Test-Running)) { Start-Jenkins }
}

function Invoke-Job {
    Require-Project
    $name = Get-JobName
    $projCi = Join-Path $Project '__dwo-forge\ci.json'
    if (Has '--remove') {
        if (Test-Path (Join-Path $Runtime 'jenkins.war')) {
            if (Test-Running) {
                $r = Invoke-Jenkins 'POST' "/job/$name/doDelete"
                if ($r.Status -in 200, 302) { Say "job $name deleted" }
            } else { Say "Jenkins is stopped; job $name stays until ci-remove or the next ci-job --remove." }
        }
        Remove-ProjectHooks $Project
        Remove-Item $projCi -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force (Join-Path $Project '__dwo-forge\ci') -ErrorAction SilentlyContinue   # hook log, push markers
        Save-Projects @(Get-Projects | Where-Object { $_.project -ne $Project })
        return
    }
    Ensure-Running
    $launcher = Join-Path $Project '__dwo-forge.bat'
    $xmlFile = Join-Path ([IO.Path]::GetTempPath()) "dwo-forge-job-$name.xml"
    & python (Join-Path $SystemDir 'tools\forge_ci.py') job-xml --project $Project --launcher $launcher --config @(Get-ConfigFiles) --out $xmlFile
    $xml = [IO.File]::ReadAllBytes($xmlFile)
    Remove-Item $xmlFile -ErrorAction SilentlyContinue
    $exists = (Invoke-Jenkins 'GET' "/job/$name/api/json?tree=name").Status -eq 200
    if ($exists) { $r = Invoke-Jenkins 'POST' "/job/$name/config.xml" $xml 'application/xml; charset=utf-8' }
    else { $r = Invoke-Jenkins 'POST' "/createItem?name=$name" $xml 'application/xml; charset=utf-8' }
    if ($r.Status -ne 200) { Fail "Jenkins rejected the job (HTTP $($r.Status)). Log: $(Join-Path $LogDir 'jenkins.log')" }
    Say "job $name $(if ($exists) { 'updated' } else { 'created' }): $(Get-BaseUrl)/job/$name/"
    $hooks = @(Install-ProjectHooks $Project)
    $ci = Get-CiConfig
    Write-JsonFile $projCi ([ordered]@{ job = $name; url = "$(Get-BaseUrl)/job/$name/"; hooks = $hooks; registeredAt = (Get-Date).ToString('o') })
    $list = @(Get-Projects | Where-Object { $_.project -ne $Project })
    $list += [pscustomobject]@{ project = $Project; job = $name }
    Save-Projects $list
    Say ("triggers: push -> $(@($ci.triggers.push.branches) -join ','); merge -> $(@($ci.triggers.merge.branches) -join ','); " +
         "new branch -> $(@($ci.triggers.branchCreate.branches) -join ','); commit -> $(@($ci.triggers.commit.branches) -join ','). " +
         "Keep $($ci.keepBuilds) builds. Change them under ""ci"" in __dwo-forge/config.json, then run ci-job again.")
}

function Send-Trigger([string]$Reason, [string[]]$Stages, [string]$Event) {
    $name = Get-JobName
    if ((Invoke-Jenkins 'GET' "/job/$name/api/json?tree=name").Status -ne 200) { Fail "No CI job for this project. Run __dwo-forge.bat ci-job first." }
    $queued = Invoke-Jenkins 'GET' "/job/$name/api/json?tree=inQueue"
    if ($queued.Status -eq 200 -and ($queued.Content | ConvertFrom-Json).inQueue) {
        Say "a build of $name is already waiting in the queue; this $Event event is merged into it"
        return
    }
    $form = @("REASON=$([uri]::EscapeDataString("[$Event] $Reason"))")
    foreach ($s in @('compile', 'lens', 'package')) { $form += "$($s.ToUpper())=$(if ($Stages -contains $s) { 'true' } else { 'false' })" }
    $r = Invoke-Jenkins 'POST' "/job/$name/buildWithParameters" ($form -join '&')
    if ($r.Status -notin 200, 201, 302) { Fail "Jenkins did not queue the build (HTTP $($r.Status))" }
    Say "queued $name ($($Stages -join ' + ')): $Reason"
}

function Invoke-Trigger {
    Require-Project
    Ensure-Running
    $ci = Get-CiConfig
    $event = Opt '--event' 'external'
    # cmd.exe splits %2..%9 at commas, so "--stages compile,lens" can arrive as "--stages compile lens":
    # take every value up to the next --flag.
    $a = @($Rest); $parts = @()
    $i = [array]::IndexOf($a, '--stages')
    if ($i -ge 0) { for ($j = $i + 1; $j -lt $a.Count -and -not ($a[$j] -like '--*'); $j++) { $parts += $a[$j] } }
    $list = if ($parts.Count) { @($parts -join ',' -split '[,+\s]+' | Where-Object { $_ }) } else { @($ci.triggers.$event.stages) }
    $bad = @($list | Where-Object { $_ -notin 'compile', 'lens', 'package' })
    if ($bad.Count) { Fail "Unknown stage: $($bad -join ', '). Use compile, lens, package." }
    if (-not $list.Count) { $list = @('compile', 'lens', 'package') }
    Send-Trigger (Opt '--reason' 'manual') $list $event
}

function Invoke-Hook {
    # Called in the background by the git hooks: <hook> <input file> <branch> <first hook argument>.
    Require-Project
    $a = @($Rest)
    $hook = $a[0]; $inputFile = $a[1]; $branch = $(if ($a.Count -gt 2) { $a[2] } else { '' }); $arg1 = $(if ($a.Count -gt 3) { $a[3] } else { '' })
    $state = Join-Path $Project '__dwo-forge\ci'
    $log = Join-Path $state 'hook.log'
    New-Item -ItemType Directory -Force $state | Out-Null
    if ($inputFile) { $inputFile = $inputFile -replace '^/([a-zA-Z])/', '$1:/' }
    try {
        # Windows PowerShell drops empty strings from native command lines, so optional values are only added when set.
        $pyArgs = @((Join-Path $SystemDir 'tools\forge_ci.py'), 'decide', '--event', $hook, '--state', $state)
        if ($branch) { $pyArgs += @('--branch', $branch) }
        if ($hook -eq 'pre-push' -and $arg1) { $pyArgs += @('--remote', $arg1) }
        if ($inputFile) { $pyArgs += @('--input', $inputFile) }
        $pyArgs += '--config'
        $pyArgs += @(Get-ConfigFiles)
        $json = Invoke-Utf8 { & python @pyArgs 2>&1 }
        if ($LASTEXITCODE -ne 0) { throw "decide failed: $($json -join ' ')" }
        # Windows PowerShell returns a JSON array as one object; enumerate it (and drop the empty-array case).
        $triggers = @(($json -join "`n") | ConvertFrom-Json | ForEach-Object { $_ } | Where-Object { $_ -and $_.event })
        Add-Content -Path $log -Encoding UTF8 -Value "$((Get-Date).ToString('s')) $hook branch=$branch -> $($triggers.Count) trigger(s)"
        foreach ($t in $triggers) {
            Add-Content -Path $log -Encoding UTF8 -Value "  [$($t.event)] $($t.reason) -> $(@($t.stages) -join '+')"
            if (-not (Test-Path (Join-Path $Runtime 'jenkins.war'))) { Add-Content -Path $log -Encoding UTF8 -Value '  skipped: local CI is not installed'; continue }
            if (-not (Test-Running)) { Start-Jenkins }
            Send-Trigger $t.reason @($t.stages) $t.event
            Add-Content -Path $log -Encoding UTF8 -Value '  queued'
        }
    } catch {
        Add-Content -Path $log -Encoding UTF8 -Value "  error: $($_.Exception.Message)"
    } finally {
        if ($inputFile) { Remove-Item $inputFile -ErrorAction SilentlyContinue }
    }
}

function Assert-Python {
    # A fresh Windows has only the Microsoft Store "python" alias, which prints an install hint and fails.
    $ErrorActionPreference = 'Continue'
    $v = $null
    if (Get-Command python -ErrorAction SilentlyContinue) {
        $v = Invoke-Utf8 { & python -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null }
        if ($LASTEXITCODE -ne 0) { $v = $null }
    }
    if (-not ("$v" -match '^(\d+)\.(\d+)$' -and ([int]$Matches[1] * 100 + [int]$Matches[2]) -ge 310)) {
        Fail ('Python 3.10 or later is required (the "python" command' + $(if ($v) { ", found $v" } else { '' }) + '). ' +
              'Install it with the python.org Windows installer (it works offline; tick "Add python.exe to PATH"), then run this again.')
    }
}
Assert-Python

switch ($Command) {
    'ci-bundle'  { Invoke-Bundle }
    'ci-install' { Invoke-Install }
    'ci-start'   { Start-Jenkins }
    'ci-stop'    { if (Test-Path $CiDir) { Stop-Jenkins } else { Say 'local CI is not installed' } }
    'ci-status'  { Invoke-Status }
    'ci-remove'  { Invoke-Remove }
    'ci-job'     { Invoke-Job }
    'ci-trigger' { Invoke-Trigger }
    'ci-hook'    { Invoke-Hook }
    'ci-hooks'   { Require-Project; if (Has '--remove') { Remove-ProjectHooks $Project } else { $null = Install-ProjectHooks $Project } }
    default      { Fail "Unknown CI command: $Command" }
}
