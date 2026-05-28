# Run from an admin PowerShell as .\tools\sign_driver.ps1 -CertName "Certificate Name"

param(
  [string] $CertName
)

$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
    Where-Object FullName -like "*\x64\signtool.exe" |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName

$inf2cat = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter Inf2Cat.exe |
    Where-Object FullName -like "*\Inf2Cat.exe" |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName

Copy-Item ".\src\driver\windows_driver\SunshineVirtualDisplayDriver.inf" -Destination ".\build-driver\src\driver\windows_driver"

& $inf2cat /os:10_X64 /driver:".\build-driver\src\driver\windows_driver"

$cat = ".\build-driver\src\driver\windows_driver\sunshinevirtualdisplaydriver.cat"

& $signtool sign /v /fd SHA256 /t http://timestamp.digicert.com /n $CertName $cat

.\build-driver\src\driver\virtualdisplay.exe driver install --inf .\build-driver\src\driver\windows_driver\SunshineVirtualDisplayDriver.inf
