# Audiobook naming and tagging

Sonix Player reads audiobooks from the `Audiobooks` folder at the root of the
microSD card. Tags are read first; the folders are the fallback. This guide is
the convention that gives the library the right title, author, series and
chapter names.

## Where the files go

The simplest layout is one folder per book:

```text
Audiobooks/
  Author Name/
    Series Name/
      Book Title/
        01 - Chapter One.mp3
        02 - Chapter Two.mp3
```

- Only `Audiobooks` is scanned, at any depth below it.
- `Author/Series` is used when the tags do not carry the author and series.
  The first folder below `Audiobooks` is the author, the second is the series.
- A book without a series can sit directly under the author:
  `Audiobooks/Author/Book Title/`.
- A single file can sit directly in `Audiobooks/Author/Series/Book Title.m4b`.
  If it has its own folder, name that folder like the book.

## One file or one folder

### One file with chapter marks

An `.m4b`, `.m4a`, `.mp4`, `.mp3`, `.opus`, `.ogg` or `.flac` file with chapter
marks inside it is a book on its own, whatever else is in the folder.

### A folder of files

Several audio files can make one book. The scanner groups the files in a folder
by their `ALBUM` tag:

- two or more files with the same `ALBUM` are one book;
- a single file with no chapter marks is a book of its own;
- files loose in the `Audiobooks` folder itself are always one book each.

`CD 1`, `CD 2`, `Disc 1`, `Disk 1` and similar subfolders are folded into the
book in the folder above them. Keep them inside the book's folder:

```text
Audiobooks/Author/Series/Book Title/
  CD 1/
    01 - Chapter One.mp3
  CD 2/
    12 - Chapter Twelve.mp3
```

For a folder book, the title is the `ALBUM` tag, or the folder name when the
tag is empty. Each part's title is the file's `TITLE` tag, or its file name.

## The tags the player reads

| What it is | ID3v2 (MP3) | Vorbis comments (Opus, Ogg, FLAC) | MP4 (M4A, M4B) | When the tag is empty |
|---|---|---|---|---|
| Book title | `TITLE` (`TIT2`) | `TITLE` | `©nam` | `ALBUM`, then the file or folder name |
| Author | `ALBUMARTIST` (`TPE2`), also `ALBUM ARTIST` / `ALBUM_ARTIST` | `ALBUMARTIST` | `aART` | `ARTIST` (`TPE1`), then the author folder |
| Series | `TXXX:SERIES`, or `MVNM` | `SERIES` | `----:SERIES`, or `©mvn` | the series folder |
| Series number | `TXXX:SERIES-PART`, `TXXX:SERIES_PART`, `TXXX:SERIESPART`, or `MVIN` | `SERIES-PART`, `SERIES_PART` or `SERIESPART` | `----:SERIES-PART`, or `©mvi` | a number at the front of the file or title |
| Summary | `COMM`, or `TXXX:DESCRIPTION` / `TXXX:SUMMARY` | `DESCRIPTION` or `SUMMARY` | `ldes`, `desc` or `©cmt` | empty |
| Album | `TALB` | `ALBUM` | `©alb` | the folder name, for a folder book |

Tag names are not case-sensitive in Vorbis comments; use the spelling above so
every tagger and the player agree.

### Author

Put the author in `ALBUMARTIST` when the file has several artists, or simply in
`ARTIST`. The player prefers `ALBUMARTIST` and falls back to `ARTIST`. If both
are empty, the first folder below `Audiobooks` is the author.

### Series and series number

Put the series in `SERIES` and the book's place in it in `SERIES-PART`. The
number may be a whole number or a decimal: `1`, `2.5`, `3/12`. The player reads
`3/12` as 3.

When the number is not tagged, the player looks at the front of the file name
and then at the title. These forms are understood:

```text
03 - The Book Title
Book 3 - The Book Title
Vol. 2 - The Book Title
Volume 2
Part 2
Tome 2
Band 2
Libro 2
#4 - The Book Title
Mistborn 1 The Final Empire
```

A bare year is not a series number: `1984` stays a title. Very large numbers
are also ignored.

### Summary

The summary page uses the first description it finds. `DESCRIPTION` and
`SUMMARY` win over an MP4 `ldes`/`desc` and over an ID3 `COMM` frame. For a
folder book, the first part that carries a summary supplies it.

## Chapter marks inside one file

A single file can carry its own chapter list. The player reads three kinds:

### MP4, M4A, M4B

Chapters come from the `chpl` box or from a chapter text track. The chapter
titles are the text track's own titles. A single mark at 0:00 is treated as
"the whole book", not as a one-chapter list.

### MP3

Chapters come from ID3v2 `CHAP` frames. Each chapter's `TIT2` frame is its
title. This is what the common audiobook splitters write.

### Opus, Ogg Vorbis, FLAC

Chapters are a pair of Vorbis comments for each chapter:

```text
CHAPTER001=00:00:00.000
CHAPTER001NAME=Chapter One
CHAPTER002=00:12:34.500
CHAPTER002NAME=Chapter Two
```

The number may start at `000` or `001` and may have any number of digits. The
time is `HH:MM:SS` with optional fractions, or `MM:SS`. A chapter without a
name is shown as "Chapter N".

## Ordering

- A book in a series is sorted by `SERIES-PART`. Books without a number go
  after the numbered ones.
- The parts of a folder book are sorted naturally by their path: `2` comes
  before `10`, and `CD 1` before `CD 2`.
- Keep the disc folders inside the book folder. A folder named `CD 1` or
  `Disc 2` is folded into the folder that contains it, so all of its files
  become part of that book.
- The player has one series field. If a book belongs to two series, choose the
  one you want to browse by; there is no second series tag.

## Examples

### One M4B with chapters

```text
Audiobooks/Brandon Sanderson/The Stormlight Archive/The Way of Kings.m4b
```

```text
TITLE=The Way of Kings
ALBUMARTIST=Brandon Sanderson
SERIES=The Stormlight Archive
SERIES-PART=1
```

### One folder per book, with numbered files

```text
Audiobooks/Author Name/Series Name/Book Title/
  01 - Chapter One.mp3
  02 - Chapter Two.mp3
```

Give every file `ALBUM=Book Title`, `ALBUMARTIST=Author Name`, `SERIES=Series
Name` and a `TITLE` for the part. The numbers in the names keep the parts in
order even when the tags are incomplete.

### A multi-disc book

```text
Audiobooks/Author Name/Series Name/Book Title/
  CD 1/
    01 - Chapter One.mp3
  CD 2/
    01 - Chapter Twelve.mp3
```

`CD 1` and `CD 2` belong to `Book Title`; do not make them separate books.

### A standalone book

```text
Audiobooks/Author Name/Book Title/Book Title.m4b
```

No `SERIES` tag and no series folder: the book appears under its author, with
no series.

## Common pitfalls

- **A tag beats a folder.** A wrong `ALBUM`, `ARTIST` or `SERIES` tag wins over
  a perfect folder tree. Check the tags before reorganising the card.
- **A shared folder needs distinct albums.** Files without an `ALBUM` in one
  folder are grouped together. Give each book its own `ALBUM`, or give each
  book its own folder.
- **A book folder should be named like the book.** If a single-file book sits
  in a folder that is not named like the file or title, the folder can be read
  as the series. Naming the folder like the book, or tagging the series,
  avoids it.
- **A year is not a number.** `1984` in a title is not a series number. Add
  `SERIES-PART` explicitly.
- **One chapter mark at zero is not a chapter list.** Give a single file at
  least two real marks.
- **Do not put a book's files loose in `Audiobooks`.** Files in that root are
  each a book of their own; the author and series folders are lost.

## Quick checklist

- [ ] The book is under `Audiobooks` (any depth below it).
- [ ] The author and series are in tags, or the folders are
      `Audiobooks/Author/Series/Book`.
- [ ] The title is in `TITLE`, or at least `ALBUM` for a folder book.
- [ ] A series book has `SERIES` and `SERIES-PART`.
- [ ] One book per folder, or a shared folder with a distinct `ALBUM` per
      book.
- [ ] A single file has chapter marks, or the folder's files are numbered and
      tagged for the parts.
- [ ] `CD 1`, `Disc 1` and similar folders are inside the book's folder.
- [ ] A number at the front of a title is not meant as a year.
