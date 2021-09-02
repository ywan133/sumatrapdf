package main

import (
	"context"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

// we delete old daily and pre-release builds. This defines how many most recent
// builds to retain
const nBuildsToRetainPreRel = 16
const nBuildsToRetainDaily = 64

const (
	// TODO: only remains because we want to update the version
	// so that people eventually upgrade to pre-release
	buildTypeDaily  = "daily"
	buildTypePreRel = "prerel"
	buildTypeRel    = "rel"
)

var (
	rel32Dir = filepath.Join("out", "rel32")
	rel64Dir = filepath.Join("out", "rel64")
)

func getRemotePaths(buildType string) []string {
	if buildType == buildTypePreRel {
		return []string{
			"software/sumatrapdf/sumatralatest.js",
			"software/sumatrapdf/sumpdf-prerelease-latest.txt",
			"software/sumatrapdf/sumpdf-prerelease-update.txt",
		}
	}

	if buildType == buildTypeDaily {
		return []string{
			"software/sumatrapdf/sumadaily.js",
			"software/sumatrapdf/sumpdf-daily-latest.txt",
			"software/sumatrapdf/sumpdf-daily-update.txt",
		}
	}

	if buildType == buildTypeRel {
		return []string{
			"software/sumatrapdf/sumarellatest.js",
			"software/sumatrapdf/release-latest.txt",
			"software/sumatrapdf/release-update.txt",
		}
	}

	panicIf(true, "Unkonwn buildType='%s'", buildType)
	return nil
}

func isValidBuildType(buildType string) bool {
	switch buildType {
	case buildTypeDaily, buildTypePreRel, buildTypeRel:
		return true
	}
	return false
}

// this returns version to be used in uploaded file names
func getVerForBuildType(buildType string) string {
	switch buildType {
	case buildTypePreRel, buildTypeDaily:
		// this is linear build number like "12223"
		return getPreReleaseVer()
	case buildTypeRel:
		// this is program version like "3.2"
		return sumatraVersion
	}
	panicIf(true, "invalid buildType '%s'", buildType)
	return ""
}

func getRemoteDir(buildType string) string {
	panicIf(!isValidBuildType(buildType), "invalid build type: '%s'", buildType)
	ver := getVerForBuildType(buildType)
	dir := "software/sumatrapdf/" + buildType + "/"
	if buildType == buildTypePreRel {
		return dir + ver + "/"
	}
	return dir
}

func newMinioSpacesClient() *MinioClient {
	bucket := "kjkpubsf"
	mc, err := minio.New("sfo2.digitaloceanspaces.com", &minio.Options{
		Creds:  credentials.NewStaticV4(os.Getenv("SPACES_KEY"), os.Getenv("SPACES_SECRET"), ""),
		Secure: true,
	})
	must(err)
	found, err := mc.BucketExists(context.Background(), bucket)
	must(err)
	panicIf(!found, "bucket '%s' doesn't exist", bucket)
	return &MinioClient{
		c:      mc,
		bucket: bucket,
	}
}

func newMinioS3Client() *MinioClient {
	bucket := "kjkpub"
	mc, err := minio.New("s3.amazonaws.com", &minio.Options{
		Creds:  credentials.NewStaticV4(os.Getenv("AWS_ACCESS"), os.Getenv("AWS_SECRET"), ""),
		Secure: true,
	})
	must(err)
	found, err := mc.BucketExists(context.Background(), bucket)
	must(err)
	panicIf(!found, "bucket '%s' doesn't exist", bucket)
	return &MinioClient{
		c:      mc,
		bucket: bucket,
	}
}

type DownloadUrls struct {
	installer64   string
	portableExe64 string
	portableZip64 string

	installer32   string
	portableExe32 string
	portableZip32 string
}

func getDownloadUrls(storage string, buildType string, ver string) *DownloadUrls {
	var prefix string
	switch storage {
	case "spaces":
		prefix = "https://kjkpubsf.sfo2.digitaloceanspaces.com/"
	case "s3":
		prefix = "https://kjkpub.s3.amazonaws.com/"
	default:
		panic(fmt.Sprintf("unknown storage '%s'", storage))
	}
	prefix += getRemoteDir(buildType)
	// zip is like .exe but can be half the size due to compression
	res := &DownloadUrls{
		installer64:   prefix + "SumatraPDF-${buildType}-${ver}-64-install.exe",
		portableExe64: prefix + "SumatraPDF-${buildType}-${ver}-64.exe",
		portableZip64: prefix + "SumatraPDF-${buildType}-${ver}-64.zip",
		installer32:   prefix + "SumatraPDF-${buildType}-${ver}-install.exe",
		portableExe32: prefix + "SumatraPDF-${buildType}-${ver}.exe",
		portableZip32: prefix + "SumatraPDF-${buildType}-${ver}.zip",
	}
	if buildType == buildTypePreRel {
		// for pre-release, ${ver} is encoded prefix
		res = &DownloadUrls{
			installer64:   prefix + "SumatraPDF-${buildType}-64-install.exe",
			portableExe64: prefix + "SumatraPDF-${buildType}-64.exe",
			portableZip64: prefix + "SumatraPDF-${buildType}-64.zip",
			installer32:   prefix + "SumatraPDF-${buildType}-install.exe",
			portableExe32: prefix + "SumatraPDF-${buildType}.exe",
			portableZip32: prefix + "SumatraPDF-${buildType}.zip",
		}
	}
	rplc := func(s *string) {
		*s = strings.Replace(*s, "${ver}", ver, -1)
		*s = strings.Replace(*s, "${buildType}", buildType, -1)
	}
	rplc(&res.installer64)
	rplc(&res.portableExe64)
	rplc(&res.portableZip64)
	rplc(&res.installer32)
	rplc(&res.portableExe32)
	rplc(&res.portableZip32)
	return res
}

// sumatrapdf/sumatralatest.js
func createSumatraLatestJs(buildType string) string {
	var appName string
	switch buildType {
	case buildTypeDaily, buildTypePreRel:
		appName = "SumatraPDF-prerel"
	case buildTypeRel:
		appName = "SumatraPDF"
	default:
		panicIf(true, "invalid buildType '%s'", buildType)
	}

	currDate := time.Now().Format("2006-01-02")
	// TODO: use
	// urls := getDownloadUrls(storage, buildType, ver)

	tmplText := `
var sumLatestVer = {{.Ver}};
var sumCommitSha1 = "{{ .Sha1 }}";
var sumBuiltOn = "{{.CurrDate}}";
var sumLatestName = "{{.Prefix}}.exe";

var sumLatestExe         = "{{.Host}}/{{.Prefix}}.exe";
var sumLatestExeZip      = "{{.Host}}/{{.Prefix}}.zip";
var sumLatestPdb         = "{{.Host}}/{{.Prefix}}.pdb.zip";
var sumLatestInstaller   = "{{.Host}}/{{.Prefix}}-install.exe";

var sumLatestExe64       = "{{.Host}}/{{.Prefix}}-64.exe";
var sumLatestExeZip64    = "{{.Host}}/{{.Prefix}}-64.zip";
var sumLatestPdb64       = "{{.Host}}/{{.Prefix}}-64.pdb.zip";
var sumLatestInstaller64 = "{{.Host}}/{{.Prefix}}-64-install.exe";
`
	ver := getVerForBuildType(buildType)
	sha1 := getGitSha1()
	d := map[string]interface{}{
		"Host":     "https://kjkpubsf.sfo2.digitaloceanspaces.com/software/sumatrapdf/" + buildType,
		"Ver":      ver,
		"Sha1":     sha1,
		"CurrDate": currDate,
		"Prefix":   appName + "-" + ver,
	}
	// for prerel, version is in path, not in name
	if buildType == buildTypePreRel {
		d["Host"] = "https://kjkpubsf.sfo2.digitaloceanspaces.com/software/sumatrapdf/" + buildType + "/" + ver
		d["Prefix"] = appName
	}
	return execTextTemplate(tmplText, d)
}

func getVersionFilesForLatestInfo(storage string, buildType string) [][]string {
	panicIf(buildType == buildTypeRel)
	remotePaths := getRemotePaths(buildType)
	var res [][]string

	{
		// *latest.js : for the website
		s := createSumatraLatestJs(buildType)
		res = append(res, []string{remotePaths[0], s})
	}

	ver := getVerForBuildType(buildType)
	{
		// *-latest.txt : for older build
		res = append(res, []string{remotePaths[1], ver})
	}

	// TODO: maybe provide download urls for both storage services
	{
		// *-update.txt : for current builds
		urls := getDownloadUrls(storage, buildType, ver)
		s := `[SumatraPDF]
Latest: ${ver}
Installer64: ${inst64}
Installer32: ${inst32}
PortableExe64: ${exe64}
PortableExe32: ${exe32}
PortableZip64: ${zip64}
PortableZip32: ${zip32}
`
		rplc := func(old, new string) {
			s = strings.Replace(s, old, new, -1)
		}
		rplc("${ver}", ver)
		rplc("${inst64}", urls.installer64)
		rplc("${inst32}", urls.installer32)
		rplc("${exe64}", urls.portableExe64)
		rplc("${exe32}", urls.portableExe32)
		rplc("${zip64}", urls.portableZip64)
		rplc("${zip32}", urls.portableZip32)

		res = append(res, []string{remotePaths[2], s})
	}

	return res
}

// we shouldn't re-upload files. We upload manifest-${ver}.txt last, so we
// consider a pre-release build already present in s3 if manifest file exists
func minioVerifyBuildNotInStorageMust(mc *MinioClient, buildType string) {
	dirRemote := getRemoteDir(buildType)
	ver := getVerForBuildType(buildType)
	fname := fmt.Sprintf("SumatraPDF-prerelease-%s-manifest.txt", ver)
	remotePath := path.Join(dirRemote, fname)
	exists := minioExists(mc, remotePath)
	panicIf(exists, "build of type '%s' for ver '%s' already exists in s3 because file '%s' exists\n", buildType, ver, remotePath)
}

func getFinalDirForBuildType(buildType string) string {
	var dir string
	switch buildType {
	case buildTypeRel:
		dir = "final-rel"
	case buildTypePreRel:
		dir = "final-prerel"
	default:
		panicIf(true, "invalid buildType '%s'", buildType)
	}
	return filepath.Join("out", dir)
}

// https://kjkpubsf.sfo2.digitaloceanspaces.com/software/sumatrapdf/prerel/SumatraPDF-prerelease-1027-install.exe etc.
func minioUploadBuildMust(mc *MinioClient, where string, buildType string) {
	timeStart := time.Now()
	defer func() {
		logf("Uploaded the build to spaces in %s\n", time.Since(timeStart))
	}()

	dirRemote := getRemoteDir(buildType)
	dirLocal := getFinalDirForBuildType(buildType)
	//verifyBuildNotInSpaces(c, buildType)

	err := minioUploadDir(mc, dirRemote, dirLocal)
	must(err)

	// for release build we don't upload files with version info
	if buildType == buildTypeRel {
		return
	}

	spacesUploadBuildUpdateInfoMust := func(buildType string) {
		files := getVersionFilesForLatestInfo(where, buildType)
		for _, f := range files {
			remotePath := f[0]
			err := minioUploadDataPublic(mc, remotePath, []byte(f[1]))
			must(err)
			logf("Uploaded to %s: '%s'\n", where, remotePath)
		}
	}

	spacesUploadBuildUpdateInfoMust(buildType)
	// TODO: for now, we also update daily version
	// to get people to switch to pre-release
	if buildType == buildTypePreRel {
		spacesUploadBuildUpdateInfoMust(buildTypeDaily)
	}
}

// "software/sumatrapdf/prerel/SumatraPDF-prerelease-11290-64-install.exe"
// =>
// 11290
func extractVersionFromName(s string) int {
	parts := strings.Split(s, "/")
	name := parts[len(parts)-1]
	// TODO: eventually we'll only need prerel- as prerelease-
	// is older naming
	name = strings.TrimPrefix(name, "SumatraPDF-prerelease-")
	name = strings.TrimPrefix(name, "SumatraPDF-prerel-")

	// TODO: temporary, for old builds in s3
	name = strings.TrimPrefix(name, "SumatraPDF-prerelase-")
	name = strings.TrimPrefix(name, "manifest-")
	name = strings.TrimPrefix(name, "manifest")
	if name == "" {
		return 0
	}

	parts = strings.Split(name, "-")
	parts = strings.Split(parts[0], ".")
	verStr := parts[0]
	ver, err := strconv.Atoi(verStr)
	if err != nil {
		// TODO: temporary, for builds uploaded with bad names
		//
		return 1
	}
	//panicIf(err != nil, "extractVersionFromName: '%s', err='%s'\n", s, err)
	return ver
}

type filesByVer struct {
	ver   int
	files []string
}

func groupFilesByVersion(files []string) []*filesByVer {
	m := map[int]*filesByVer{}
	for _, f := range files {
		ver := extractVersionFromName(f)
		i := m[ver]
		if i == nil {
			i = &filesByVer{
				ver: ver,
			}
			m[ver] = i
		}
		i.files = append(i.files, f)
	}
	res := []*filesByVer{}
	for _, v := range m {
		res = append(res, v)
	}
	sort.Slice(res, func(i, j int) bool {
		return res[i].ver > res[j].ver
	})
	return res
}

func minioDeleteOldBuildsPrefix(mc *MinioClient, buildType string) {
	panicIf(buildType == buildTypeRel, "can't delete release builds")

	nBuildsToRetain := nBuildsToRetainDaily
	if buildType == buildTypePreRel {
		nBuildsToRetain = nBuildsToRetainPreRel
	}
	remoteDir := getRemoteDir(buildType)

	opts := minio.ListObjectsOptions{
		Prefix:    remoteDir,
		Recursive: true,
	}
	objectsCh := mc.c.ListObjects(ctx(), mc.bucket, opts)
	var keys []string
	for f := range objectsCh {
		keys = append(keys, f.Key)
		//fmt.Printf("key: %s\n", f.Key)
	}

	uri := minioURLForPath(mc, remoteDir)
	logf("%d files under '%s'\n", len(keys), uri)
	byVer := groupFilesByVersion(keys)
	for i, v := range byVer {
		deleting := (i >= nBuildsToRetain)
		if deleting {
			logf("%d, deleting\n", v.ver)
			for _, key := range v.files {
				logf("  %s deleting\n", key)
				err := minioRemove(mc, key)
				must(err)
			}
		}
	}
}

func spacesDeleteOldBuilds() {
	mc := newMinioSpacesClient()
	minioDeleteOldBuildsPrefix(mc, buildTypePreRel)
	//spacesDeleteOldBuildsPrefix(buildTypeDaily)
}

func s3DeleteOldBuilds() {
	mc := newMinioS3Client()
	minioDeleteOldBuildsPrefix(mc, buildTypePreRel)
	// TODO: we can remove them completely
	//s3DeleteOldBuildsPrefix(buildTypeDaily)
}