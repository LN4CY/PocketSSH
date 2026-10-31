param(
    [string]$Port = ""
)

$env:IDF_TOOLS_PATH = "E:\proj\sdk\.espressif"
$env:PATH = "E:\proj\sdk\.espressif\tools\idf-python\3.11.2;$env:PATH"
. E:\proj\sdk\v5.4.1\esp-idf\export.ps1

if ($Port -ne "") {
    idf.py -p $Port flash
} else {
    idf.py flash
}
