package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

var (
	apptranslatoServer  = "https://www.apptranslator.org"
	translationsDir     = "translations"
	translationsTxtPath = filepath.Join(translationsDir, "translations.txt")
)

func getTransSecret() string {
	panicIf(transUploadSecret == "", "must set TRANS_UPLOAD_SECRET env variable or in .env file")
	return transUploadSecret
}

// sometimes people press enter at the end of the translation
// we should fix it in apptranslator.org but for now fix it here
func fixTranslation(s string) string {
	s = strings.TrimSpace(s)
	s = strings.TrimSuffix(s, `\n`)
	s = strings.TrimSuffix(s, `\r`)
	s = strings.TrimSuffix(s, `\n`)
	s = strings.TrimSpace(s)
	return s
}

type BadTranslation struct {
	currString string
	orig       string
	fixed      string
}

var badTranslatins []BadTranslation

func printBadTranslations() {
	sort.Slice(badTranslatins, func(i, j int) bool {
		return badTranslatins[i].orig < badTranslatins[j].orig
	})
	currLang := ""
	for _, bt := range badTranslatins {
		lang := strings.Split(bt.orig, ":")[0]
		if lang != currLang {
			currLang = lang
			uri := "https://www.apptranslator.org/app/SumatraPDF/" + lang
			fmt.Printf("\n%s\n", uri)
		}
		fmt.Printf("%s\n  '%s' => '%s'\n", bt.currString, bt.orig, bt.fixed)
	}
}

func fixTranslations(d []byte) []byte {
	var b bytes.Buffer
	a := strings.Split(string(d), "\n")
	currString := ""

	for _, s := range a {
		if strings.HasPrefix(s, ":") {
			currString = s[1:]
			b.WriteString(s + "\n")
			continue
		}
		fixed := fixTranslation(s)
		if s != fixed {
			push(&badTranslatins, BadTranslation{
				currString: currString,
				orig:       s,
				fixed:      fixed,
			})
		}
		b.WriteString(fixed + "\n")
	}
	res := b.Bytes()
	return res[:len(res)-1] // remove last \n
}

func downloadTranslationsMust() []byte {
	timeStart := time.Now()
	defer func() {
		fmt.Printf("downloadTranslations() finished in %s\n", time.Since(timeStart))
	}()
	strs := extractStringsFromCFilesNoPaths()
	sort.Strings(strs)
	fmt.Printf("uploading %d strings for translation\n", len(strs))
	secret := getTransSecret()
	uri := apptranslatoServer + "/api/dltransfor?app=SumatraPDF&secret=" + secret
	s := strings.Join(strs, "\n")
	body := strings.NewReader(s)
	req, err := http.NewRequest(http.MethodPost, uri, body)
	must(err)
	client := http.DefaultClient
	rsp, err := client.Do(req)
	must(err)
	panicIf(rsp.StatusCode != http.StatusOK)
	d, err := io.ReadAll(rsp.Body)
	must(err)
	return d
}

/*
The file looks like:

AppTranslator: SumatraPDF
608ebc3039db395ff05d3d5d950afdd65a233c58
:&About
af:&Omtrent
am:&Ծրագրի մասին
*/
func generateGoodSubset(d []byte) {
	a := strings.Split(string(d), "\n")
	a = a[2:]
	perLang := make(map[string]map[string]string)
	allStrings := []string{}
	currString := "" // string we're currently processing

	addLangTrans := func(lang, trans string) {
		m := perLang[lang]
		if m == nil {
			m = make(map[string]string)
			perLang[lang] = m
		}
		m[currString] = trans
	}

	goodLangs := []string{}
	fullyTranslated := []string{}
	notTranslated := []string{}
	// build perLang maps
	for _, s := range a {
		if len(s) == 0 {
			// can happen at the end of the file
			continue
		}
		if strings.HasPrefix(s, ":") {
			currString = s[1:]
			allStrings = append(allStrings, currString)
			continue
		}
		parts := strings.SplitN(s, ":", 2)
		lang := parts[0]
		panicIf(len(lang) > 5)
		panicIf(len(parts) == 1, "parts: '%s'\n", parts)
		trans := parts[1]
		addLangTrans(lang, trans)
	}

	nStrings := len(allStrings)
	langsToSkip := map[string]bool{}
	for lang, m := range perLang {
		a := []string{}
		sort.Slice(allStrings, func(i, j int) bool {
			s1 := allStrings[i]
			s2 := allStrings[j]
			s1IsTranslated := m[s1] != ""
			s2IsTranslated := m[s2] != ""
			if !s1IsTranslated && s2IsTranslated {
				return true
			}
			if s1IsTranslated && !s2IsTranslated {
				return false
			}
			return s1 < s2
		})
		for _, s := range allStrings {
			a = append(a, ":"+s)
			trans := m[s]
			panicIf(strings.Contains(trans, "\n"))
			if len(trans) == 0 {
				continue
			}
			a = append(a, trans)
		}
		// note: no longer writing per-language files
		// too much churn in the repo
		if false {
			s := strings.Join(a, "\n")
			path := filepath.Join(translationsDir, lang+".txt")
			writeFileMust(path, []byte(s))
		}
		nMissing := nStrings - len(m)
		skipStr := ""
		if nMissing > 100 {
			skipStr = "  SKIP"
			langsToSkip[lang] = true
			notTranslated = append(notTranslated, lang)
		} else {
			if nMissing == 0 {
				fullyTranslated = append(fullyTranslated, lang)
			} else {
				goodLangs = append(goodLangs, lang)
			}
		}
		if nMissing > 0 {
			logf("Lang %s, missing: %d%s\n", lang, nMissing, skipStr)
		}
	}

	// write translations-good.txt with langs that don't miss too many translations
	sort.Strings(allStrings)
	// for backwards compat with translations.txt first 2 lines
	// are skipped by ParseTranslationsTxt()
	a = []string{
		"AppTranslator: SumatraPDF",
		"AppTranslator: SumatraPDF",
	}
	// sort languages for better diffs of translations-good.txt
	sortedLangs := []string{}
	for lang := range perLang {
		if langsToSkip[lang] {
			continue
		}
		sortedLangs = append(sortedLangs, lang)
	}
	sort.Strings(sortedLangs)

	for _, s := range allStrings {
		a = append(a, ":"+s)
		for _, lang := range sortedLangs {
			m := perLang[lang]
			trans := m[s]
			panicIf(strings.Contains(trans, "\n"))
			if len(trans) == 0 {
				continue
			}
			a = append(a, lang+":"+trans)
		}
	}
	s := strings.Join(a, "\n")
	path := filepath.Join(translationsDir, "translations-good.txt")
	writeFileMust(path, []byte(s))
	logf("Wrote %s of size %d\n", path, len(s))
	logf("not translated langs: %v\n", notTranslated)
	logf("good langs: %v\n", goodLangs)
	logf("fully translated langs: %v\n", fullyTranslated)
}

func downloadTranslations() bool {
	d := downloadTranslationsMust()
	d = fixTranslations(d)

	printBadTranslations()

	path := filepath.Join(translationsDir, "translations.txt")
	curr := readFileMust(path)
	if bytes.Equal(d, curr) {
		fmt.Printf("Translations didn't change\n")
		//return false
	}

	generateGoodSubset(d)

	// TODO: save ~400k in uncompressed binary by
	// saving as gzipped and embedding that in the exe
	//u.WriteFileGzipped(translationsTxtPath+".gz", d)
	writeFileMust(path, d)
	logf("Wrote %s of size %d\n", path, len(d))

	return false
}

// TODO:
// - generate translations/status.md file that shows how many
//   strings untranslated per language and links to their files
// - do this when updating from soource:
//	 - read current per-lang translations
//   - extract strings from source
//   - remove no longer needed
//   - add new ones
//   - re-save per-lang files
//   - save no longer needeed in obsolete.txt
