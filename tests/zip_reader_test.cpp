// Zips: the reader (stored and deflated entries, the CRC check, what it
// refuses) and taking a LUT out of a camera maker's zip download
// (projectfolder::lutsInZip / importLutFromZip: which .cube is offered first,
// where it is unpacked to). The zips are made here with zlib.
// Standalone; the exit code is the failure count.

#include "support/ModelTest.h"
#include "support/TestZip.h"

#include "ZipReader.h"

#include <QCoreApplication>
#include <QStandardPaths>

#include <string>
#include <vector>

namespace {

// A size^3 identity .cube, `pad` comment lines making it bigger than others.
std::string cube(int size, int pad = 0)
{
    std::string s = "TITLE \"test\"\n";
    for (int i = 0; i < pad; ++i)
        s += "# padding\n";
    s += "LUT_3D_SIZE " + std::to_string(size) + "\n";
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r)
                s += std::to_string(r / float(size - 1)) + " " + std::to_string(g / float(size - 1)) +
                     " " + std::to_string(b / float(size - 1)) + "\n";
    return s;
}

void writeBytes(const QString &path, const std::string &bytes)
{
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(bytes.data(), qint64(bytes.size()));
}

QByteArray readBytes(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void testReader()
{
    const std::string text = cube(5);
    const std::string zip = testzip::makeZip({{"pack/", "", false},
                                     {"pack/a.cube", text, true},
                                     {"pack/b.txt", "stored as is", false}});
    std::string err;
    auto archive = zip::Archive::fromBytes(zip, &err);
    CHECK(archive && err.empty());
    if (!archive)
        return;
    CHECK(archive->entries().size() == 3);
    CHECK(archive->entries()[0].isDirectory() && !archive->entries()[1].isDirectory());
    CHECK(archive->entries()[1].name == "pack/a.cube" && archive->entries()[1].size == text.size());
    std::string out;
    CHECK(archive->read(1, &out, &err) && out == text);
    CHECK(archive->read(2, &out, &err) && out == "stored as is");
    CHECK(archive->read(0, &out, &err) && out.empty());
    CHECK(!archive->read(3, &out, &err));

    // An archive comment after the end record is searched past.
    std::string commented = zip;
    commented[commented.size() - 2] = 5;
    commented += "hello";
    CHECK(zip::Archive::fromBytes(commented) != nullptr);

    // A flipped byte in the stored data fails the CRC check.
    std::string damaged = zip;
    const std::size_t at = damaged.find("stored as is");
    damaged[at] = 'S';
    archive = zip::Archive::fromBytes(damaged, &err);
    CHECK(archive && !archive->read(2, &out, &err) && out.empty() && !err.empty());

    // Not zips.
    err.clear();
    CHECK(!zip::Archive::fromBytes("", &err) && !err.empty());
    CHECK(!zip::Archive::fromBytes(text, &err));
    CHECK(!zip::Archive::fromBytes(zip.substr(0, zip.size() - 30), &err)); // cut short
    // Encrypted entries are refused up front.
    err.clear();
    CHECK(!zip::Archive::fromBytes(testzip::makeZip({{"a.cube", text}}, 1), &err) &&
          err.find("password") != std::string::npos);
    // A file on disk.
    QTemporaryDir tmp;
    writeBytes(tmp.path() + "/pack.zip", zip);
    CHECK(zip::Archive::open((tmp.path() + "/pack.zip").toStdString()) != nullptr);
    CHECK(!zip::Archive::open((tmp.path() + "/missing.zip").toStdString(), &err));
}

void testLutsInZip()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString zipFile = tmp.path() + "/X6_I-Log_LUTs.zip";
    // As Insta360's X6 pack: two targets in two sizes, plus macOS's extras.
    writeBytes(zipFile, testzip::makeZip({{"X6_I-Log_LUTs/", "", false},
                                 {"X6_I-Log_LUTs/X6_I-Log_to_Rec2020_HLG_s65.cube", cube(4, 50)},
                                 {"__MACOSX/X6_I-Log_LUTs/._X6_I-Log_to_Rec2020_HLG_s65.cube", "x"},
                                 {"X6_I-Log_LUTs/.DS_Store", "x"},
                                 {"X6_I-Log_LUTs/X6_I-Log_to_Rec709_s33.cube", cube(2)},
                                 {"X6_I-Log_LUTs/X6_I-Log_to_Rec709_s65.CUBE", cube(3)},
                                 {"X6_I-Log_LUTs/readme.txt", "x"}}));
    int preferred = -1;
    QString err;
    const QList<projectfolder::ZippedLut> luts = projectfolder::lutsInZip(zipFile, &preferred, &err);
    CHECK(luts.size() == 3 && err.isEmpty());
    if (luts.size() != 3)
        return;
    CHECK(luts[0].name == "X6_I-Log_to_Rec2020_HLG_s65.cube");
    CHECK(luts[1].entry == "X6_I-Log_LUTs/X6_I-Log_to_Rec709_s33.cube");
    // The largest Rec.709 one, though the HLG one is larger still.
    CHECK(preferred == 2);

    // No 709 in any name (a LUT per lens): the largest.
    const QString lenses = tmp.path() + "/ONE-RS-LUT.zip";
    writeBytes(lenses, testzip::makeZip({{"ONE-RS-LUT/1-INCH_33.CUBE", cube(2)},
                                {"ONE-RS-LUT/1-INCH_65.CUBE", cube(3)},
                                {"ONE-RS-LUT/4K-WIDE-ANGLE_33.CUBE", cube(2, 1)}}));
    CHECK(projectfolder::lutsInZip(lenses, &preferred).size() == 3 && preferred == 1);

    // No .cube, not a zip.
    const QString empty = tmp.path() + "/empty.zip";
    writeBytes(empty, testzip::makeZip({{"__MACOSX/._a.cube", "x"}, {"notes.txt", "x"}}));
    CHECK(projectfolder::lutsInZip(empty, &preferred, &err).isEmpty() && preferred == -1 &&
          err.contains(".cube"));
    err.clear();
    CHECK(projectfolder::lutsInZip(tmp.path() + "/missing.zip", nullptr, &err).isEmpty() &&
          !err.isEmpty());
}

void testImportLutFromZip()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const std::string x5 = cube(3);
    const QString zipFile = tmp.path() + "/Insta360 X5 LUT.zip";
    writeBytes(zipFile, testzip::makeZip({{"Insta360 X5 LUT/X5_I-Log_To_Rec.709_V1.0.cube", x5},
                                 {"Insta360 X5 LUT/broken.cube", "LUT_3D_SIZE 2\n0 0 0\n"},
                                 {"Insta360 X5 LUT/._hidden.cube", x5}}));
    QString err;
    const QString file = projectfolder::create(tmp.path(), "Ride", &err);
    CHECK(!file.isEmpty());

    const QString entry = "Insta360 X5 LUT/X5_I-Log_To_Rec.709_V1.0.cube";
    const QString lut = projectfolder::importLutFromZip(file, zipFile, entry, &err);
    CHECK(lut == tmp.path() + "/Ride/luts/X5_I-Log_To_Rec.709_V1.0.cube");
    CHECK(readBytes(lut) == QByteArray::fromStdString(x5));
    CHECK(projectfolder::importLutFromZip(file, zipFile, entry) == lut); // the same again

    // Not a LUT we can use, not in the zip, a hidden file: nothing written.
    err.clear();
    CHECK(projectfolder::importLutFromZip(file, zipFile, "Insta360 X5 LUT/broken.cube", &err)
              .isEmpty() &&
          err.contains("broken.cube"));
    CHECK(!QFileInfo::exists(tmp.path() + "/Ride/luts/broken.cube"));
    CHECK(projectfolder::importLutFromZip(file, zipFile, "nope.cube", &err).isEmpty());
    CHECK(projectfolder::importLutFromZip(file, zipFile, "Insta360 X5 LUT/._hidden.cube").isEmpty());

    // A project without a folder: into the app's data folder.
    const QString loose = projectfolder::importLutFromZip(tmp.path() + "/loose.orp", zipFile, entry);
    CHECK(loose == projectfolder::unpackedLutsDir() + "/X5_I-Log_To_Rec.709_V1.0.cube");
    CHECK(readBytes(loose) == QByteArray::fromStdString(x5));
    CHECK(projectfolder::importLutFromZip(QString(), zipFile, entry) == loose);
    QFile::remove(loose);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true); // unpackedLutsDir() under ~/.qttest
    testReader();
    testLutsInZip();
    testImportLutFromZip();
    return modeltest::finish("zip_reader_test");
}
