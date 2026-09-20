param(
    [ValidateSet('plan', 'up', 'status', 'stop', 'shell', 'exec')]
    [string]$Action = 'status',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Command
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..')).TrimEnd('\', '/')
$containerName = 'praesidium-humanitatis-tft'
$image = 'debian@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443'
$containerRoot = '/workspace/praesidium-humanitatis'
$manager = 'praesidium-humanitatis'
function Show-Configuration {
    Write-Host 'Container configuration:'
    Write-Host "Container: $containerName"
    Write-Host "Image: $image"
    Write-Host 'Command: /bin/bash (interactive, TTY); working directory: /workspace/praesidium-humanitatis'
    Write-Host "Bind mount: $projectRoot -> $containerRoot (read/write)"
    Write-Host 'Named volumes: none; published ports: none; restart policy: no'
    Write-Host 'GPU access: all NVIDIA GPUs; other devices: none; shared memory: 1 GiB'
    Write-Host 'Privileged: false; network: default bridge; configuration label: 1'
}

function Invoke-Docker {
    & docker @args
    if ($LASTEXITCODE -ne 0) { throw "Docker failed (exit $LASTEXITCODE); state is preserved." }
}

function Get-ManagedContainer {
    $ids = @(Invoke-Docker ps -aq --no-trunc --filter "name=^/$containerName`$")
    if ($ids.Count -eq 0) { return $null }
    if ($ids.Count -ne 1) { throw "Expected one exact container name; found $($ids.Count). Nothing changed." }
    $info = (Invoke-Docker inspect --type container $ids[0] | ConvertFrom-Json)[0]
    $mounts = @($info.Mounts)
    $gpuRequests = @($info.HostConfig.DeviceRequests)
    $portBindings = @($info.HostConfig.PortBindings.PSObject.Properties)
    $expectedImageId = Invoke-Docker image inspect $image --format '{{.Id}}'
    $mountSource = $projectRoot
    $mountTarget = $containerRoot
    $labelPrefix = 'org.praesidium-humanitatis'
    $expectedManager = $manager
    $valid = $info.Id -eq $ids[0] -and
        $info.Name -ceq "/$containerName" -and
        $info.Config.Labels."$labelPrefix.managed-by" -eq $expectedManager -and
        $info.Config.Labels."$labelPrefix.project-root" -eq $mountSource -and
        $info.Config.Labels."$labelPrefix.configuration" -eq '1' -and
        $info.Config.Labels."$labelPrefix.libtorch" -eq '2.6.0+cu124-cxx11' -and
        $info.Config.Image -eq $image -and $info.Image -eq $expectedImageId -and
        $info.Config.WorkingDir -ceq $mountTarget -and
        @($info.Config.Cmd).Count -eq 1 -and $info.Config.Cmd[0] -ceq '/bin/bash' -and
        -not $info.Config.Entrypoint -and
        $info.Config.OpenStdin -and $info.Config.Tty -and
        $mounts.Count -eq 1 -and $mounts[0].Type -eq 'bind' -and
        $mounts[0].Source -eq $mountSource -and
        $mounts[0].Destination -ceq $mountTarget -and $mounts[0].RW -and
        $info.HostConfig.ShmSize -eq 1073741824 -and
        $info.HostConfig.RestartPolicy.Name -eq 'no' -and
        $info.HostConfig.RestartPolicy.MaximumRetryCount -eq 0 -and
        -not $info.HostConfig.Privileged -and -not $info.HostConfig.CapAdd -and
        $info.HostConfig.NetworkMode -in @('default', 'bridge') -and
        -not $info.HostConfig.PublishAllPorts -and
        $gpuRequests.Count -eq 1 -and $gpuRequests[0].Count -eq -1 -and
        $gpuRequests[0].Driver -in @('', 'nvidia') -and
        -not $gpuRequests[0].DeviceIDs -and
        @($gpuRequests[0].Capabilities).Count -eq 1 -and
        (@($gpuRequests[0].Capabilities[0]) -join ',') -eq 'gpu' -and
        -not $info.HostConfig.Devices -and $portBindings.Count -eq 0
    if (-not $valid) {
        throw "Container '$containerName' ($($info.Id)) has unmanaged or mismatched configuration; preserved without changes."
    }
    return $info
}

if ($Action -eq 'plan') { Show-Configuration; return }
$container = Get-ManagedContainer
if ($Action -eq 'up') {
    Show-Configuration
    if ($null -eq $container) {
        $available = @(Invoke-Docker image ls -q $image)
        if ($available.Count -eq 0) { Invoke-Docker pull $image | Out-Host }
        # Never replace an existing container or delete any Docker object.
        $createdId = Invoke-Docker create --name $containerName --interactive --tty `
            --gpus all --shm-size 1g --restart no `
            --label "org.praesidium-humanitatis.managed-by=$manager" `
            --label "org.praesidium-humanitatis.project-root=$projectRoot" `
            --label 'org.praesidium-humanitatis.configuration=1' `
            --label 'org.praesidium-humanitatis.libtorch=2.6.0+cu124-cxx11' `
            --mount "type=bind,source=$projectRoot,target=$containerRoot" `
            --workdir $containerRoot $image /bin/bash
        $container = Get-ManagedContainer
        if ($null -eq $container -or $container.Id -ne $createdId) {
            throw 'Created container identity could not be verified; state is preserved for inspection.'
        }
    }
    $verifiedId = $container.Id
    if (-not $container.State.Running) { Invoke-Docker start $verifiedId | Out-Host }
    $container = Get-ManagedContainer
    if ($null -eq $container -or $container.Id -ne $verifiedId -or -not $container.State.Running) {
        throw 'Container start or identity verification failed; state is preserved for inspection.'
    }
    Write-Output "$containerName $($container.Id) $($container.State.Status)"
    Write-Output 'Container configuration verified. Install/check environment with: exec bash tools/temporal-fusion-transformer/environment/setup.sh'
    return
}
if ($null -eq $container) {
    if ($Action -eq 'status') { Write-Output "$containerName absent"; return }
    throw 'Container is absent. Review the plan, then run tools/temporal-fusion-transformer/environment/container.ps1 up.'
}
switch ($Action) {
    'status' { Write-Output "$containerName $($container.Id) $($container.State.Status)" }
    'stop' { if ($container.State.Running) { Invoke-Docker stop $container.Id } }
    'shell' {
        if (-not $container.State.Running) { throw 'Container is stopped. Run tools/temporal-fusion-transformer/environment/container.ps1 up.' }
        Invoke-Docker exec -it --workdir $containerRoot $container.Id /bin/bash
    }
    'exec' {
        if (-not $container.State.Running) { throw 'Container is stopped. Run tools/temporal-fusion-transformer/environment/container.ps1 up.' }
        if (-not $Command) { throw 'Supply a command using -Command @(...) after -Action exec.' }
        Invoke-Docker exec --workdir $containerRoot $container.Id @Command
    }
}
