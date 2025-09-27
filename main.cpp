#include "qapplication.h"
#include "qglobal.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ioapi.h>
#include <mz.h>
#include <mz_strm.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mz_zip.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <vector>

#include <QApplication>
#include <QPushButton>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "res.cpp"

#include <cerrno>
#include <system_error>

namespace fs = std::filesystem;

struct FileError : std::exception {
  const char *message;
  FileError(const char *m = "An I/O error ocurred") { message = m; }
  const char *what() const noexcept override { return message; }
};

static int32_t my_stream_is_open_cb(void *stream);
static int32_t my_stream_open_cb(void *stream, const char *path, int32_t mode);
static int32_t my_stream_read_cb(void *stream, void *buf, int32_t size);
static int32_t my_stream_seek_cb(void *stream, int64_t offset, int32_t origin);
static int32_t my_stream_write_cb(void *stream, const void *buf, int32_t size);
static int64_t my_stream_tell_cb(void *stream);
static int32_t my_stream_close_cb(void *stream);
static int32_t my_stream_error_cb(void *stream);
static void *my_stream_create_cb(void);
static void my_stream_destroy_cb(void **stream);
static int32_t my_stream_get_prop_int64_cb(void *stream, int32_t prop,
                                           int64_t *value);
static int32_t my_stream_set_prop_int64_cb(void *stream, int32_t prop,
                                           int64_t value);
std::string generic_error_msg() {
  return std::error_code(errno, std::generic_category()).message();
}

std::string random_suffix(int length = 6) {
  static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string s;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
  for (int i = 0; i < length; ++i)
    s += chars[dis(gen)];
  return s;
}

fs::path create_temp_work_dir(const std::string &prefix = "myapp") {
  fs::path base = fs::temp_directory_path();
  fs::path work_dir;

  do {
    work_dir = base / (prefix + "-" + random_suffix());
  } while (fs::exists(work_dir));

  fs::create_directory(work_dir);
  return work_dir;
}

struct Mystream {
  struct Error : std::exception {
    std::string message;
    Error(std::string m = "A stream error ocurred\n") { message = m; }
    const char *what() const noexcept override { return message.c_str(); }
  };

  mz_stream_vtbl vtbl;

  struct Ctx {
    mz_stream_vtbl *vtbl;
    struct mz_stream_s *base;
    Mystream *_strm;
  };
  Ctx strm;
  std::map<int32_t, int64_t> props;

  struct Part {
    off_t begin;
    off_t end;
    off_t file_size;
    FILE *file;

    static size_t init(Part *p, size_t begin, const char *file_path) {
      p->file = fopen(file_path, "rb");
      if (p->file == nullptr) {
        std::string err_msg =
            std::error_code(errno, std::generic_category()).message();
        err_msg.append(": ");
        err_msg.append(file_path);
        throw Error(err_msg);
      }

      int res = fseek(p->file, 0, SEEK_END);
      if (res != 0) {
        throw Error(generic_error_msg() + ": " + file_path);
      }
      off_t file_size = ftello(p->file);
      if (file_size == (off_t)-1) {
        throw Error(generic_error_msg() + ": " + file_path);
      }
      res = fseek(p->file, 0, SEEK_SET);
      if (res != 0) {
        throw Error(generic_error_msg() + ": " + file_path);
      }

      p->begin = begin;
      p->end = begin + file_size;
      p->file_size = file_size;

      return p->end;
    }

    bool has(off_t offt) noexcept { return offt >= begin and offt < end; }

    int seek_to(off_t offt) noexcept { return fseek(file, offt, SEEK_SET); }

    off_t local_offt(off_t global_offt) noexcept {
      if (!has(global_offt)) {
        return -1;
      }
      return global_offt - begin;
    }

    int32_t read(void *buf, int32_t len, off_t global_offt) noexcept {
      off_t lofft = local_offt(global_offt);
      if (lofft == -1) {
        return -1;
      }
      if (seek_to(lofft) == -1) {
        return -1;
      };
      int32_t read = 0;
      while (read < len) {
        size_t n = fread((char *)buf + read, 1, len - read, file);
        read += n;
        if (n == 0) {
          if (ferror(file)) {
            return -1;
          }
          break;
        }
      }
      return read;
    }
  };

  std::vector<Part> parts;
  off_t whole_offt;
  off_t whole_size;

  Mystream(std::string *part_path) {
    memset(&vtbl, 0, sizeof(vtbl));

    vtbl.open = my_stream_open_cb;
    vtbl.is_open = my_stream_is_open_cb;
    vtbl.read = my_stream_read_cb;
    vtbl.seek = my_stream_seek_cb;
    vtbl.write = my_stream_write_cb;
    vtbl.tell = my_stream_tell_cb;
    vtbl.error = my_stream_error_cb;
    vtbl.close = my_stream_close_cb;
    vtbl.create = my_stream_create_cb;
    vtbl.destroy = my_stream_destroy_cb;
    vtbl.set_prop_int64 = my_stream_set_prop_int64_cb;
    vtbl.get_prop_int64 = my_stream_get_prop_int64_cb;

    strm.vtbl = &vtbl;
    strm._strm = this;

    Part tmp;
    off_t offt = 0;
    whole_size = 0;

    // auto it = part_paths->begin();

    // for (size_t i = 0; i < part_paths->size(); i++) {
    off_t new_offt = Part::init(&tmp, offt, part_path->c_str());
    parts.push_back(tmp);
    offt = new_offt;
    whole_size += tmp.file_size;
    // std::advance(it, 1);
    // }

    whole_offt = 0;
  }

  int64_t get_prop(int32_t key) { return props[key]; }

  void set_prop(int32_t key, int64_t value) { props[key] = value; }

  Part *find_part_wofft(off_t offt) {
    for (size_t i = 0; i < parts.size(); i++) {
      Part *tmp = &parts[i];
      if (tmp->has(offt)) {
        return tmp;
      }
    }
    return nullptr;
  }

  int32_t open(const char *path, int32_t mode) {
    (void)path;
    (void)mode;
    assert(false);
    return -1;
  }

  int32_t read(void *buf, int32_t size) {
    Part *part = find_part_wofft(whole_offt);
    if (part == nullptr)
      return -1;
    int32_t n = part->read(buf, size, whole_offt);
    whole_offt += n;
    return n;
  }

  int32_t write(const void *buf, int32_t size) {
    (void)buf;
    (void)size;
    assert(false);
    return -1;
  }

  int32_t seek(int64_t offset, int32_t origin) {
    switch (origin) {
    case MZ_SEEK_SET:
      if (offset > whole_size)
        return -1;
      this->whole_offt = offset;
      break;
    case MZ_SEEK_CUR:
      if (offset > 0) {
        if (whole_offt + offset > whole_size)
          return -1;
        this->whole_offt += offset;
      } else {
        if (whole_offt < offset)
          return -1;
        this->whole_offt -= offset;
      }
      break;
    case MZ_SEEK_END:
      if (offset > whole_size)
        return -1;
      this->whole_offt = whole_size - offset;
      break;
    default:
      assert(false && "unknow origin");
    }
    return 0;
  }

  int64_t tell() { return whole_offt; }

  int32_t close() {
    assert(false);
    return -1;
  }

  int32_t error() {
    assert(false);
    return -1;
  }

  void *create() { assert(false); }

  void *destroy() { assert(false); }

  mz_stream *get_mz_stream() { return (mz_stream *)&strm; }
};

static int32_t my_stream_get_prop_int64_cb(void *stream, int32_t prop,
                                           int64_t *value) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  if (value != nullptr) {
    *value = ctx->_strm->get_prop(prop);
  }
  return 0;
}

static int32_t my_stream_set_prop_int64_cb(void *stream, int32_t prop,
                                           int64_t value) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  ctx->_strm->set_prop(prop, value);
  return 0;
}

static void *my_stream_create_cb(void) {
  // printf("%s()\n", __FUNCTION__);
  assert(false);
  return nullptr;
}

static void my_stream_destroy_cb(void **stream) {
  // printf("%s()\n", __FUNCTION__);
  (void)stream;
  assert(false);
}

static int32_t my_stream_close_cb(void *stream) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->close();
}

static int32_t my_stream_error_cb(void *stream) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->error();
}

static int64_t my_stream_tell_cb(void *stream) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->tell();
}

static int32_t my_stream_write_cb(void *stream, const void *buf, int32_t size) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->write(buf, size);
}

static int32_t my_stream_seek_cb(void *stream, int64_t offset, int32_t origin) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->seek(offset, origin);
}

static int32_t my_stream_read_cb(void *stream, void *buf, int32_t size) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->read(buf, size);
}

static int32_t my_stream_is_open_cb(void *stream) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->parts.empty();
}

static int32_t my_stream_open_cb(void *stream, const char *path, int32_t mode) {
  // printf("%s()\n", __FUNCTION__);
  Mystream::Ctx *ctx = (Mystream::Ctx *)stream;
  return ctx->_strm->open(path, mode);
}
uint64_t write_file(FILE *f, const char *buf, uint64_t len) {
  uint64_t written = 0;
  while (written < len) {
    uint64_t amnt = len - written;
    uint64_t n = fwrite(buf + written, 1, amnt, f);
    if (n != amnt) {
      if (ferror(f)) {
        throw FileError();
      }
      break;
    }
    written += n;
  }
  return written;
}

struct Archive {
  struct Error : std::exception {
    const char *message;
    Error(const char *m = "An error ocurred while opening the archive") {
      message = m;
    }
    const char *what() const noexcept override { return message; }
  };

  void *zip;
  Mystream *stream;
  uint64_t num_entries;

  struct Entry {
    void *parent;
    mz_zip_file *entry;
    Archive *archive;

    enum { RBUFSIZ = 4096 * 8 };

    int read_open() {
      int res = mz_zip_entry_read_open(parent, 0, nullptr);
      if (res != MZ_OK)
        return res;
      return mz_zip_entry_get_info(parent, &entry);
    }

    int read_close() {
      return mz_zip_entry_read_close(parent, &entry->crc,
                                     &entry->compressed_size,
                                     &entry->uncompressed_size);
    }

    const char *get_name() { return entry->filename; }

    bool is_dir() { return !mz_zip_entry_is_dir(parent); }

    bool is_symlink() { return !mz_zip_entry_is_symlink(parent); }

    bool canceled() { return archive->cancel; }

    int write_to_file(FILE *file) noexcept {
      char *rbuf = (char *)malloc(RBUFSIZ);
      if (rbuf == nullptr) {
        return -1;
      }

      int64_t rem_entry = entry->uncompressed_size;
      int32_t read_entry, write_entry;

      while (rem_entry > 0 and !canceled()) {
        read_entry = mz_zip_entry_read(parent, rbuf, RBUFSIZ);
        if (read_entry < 0) {
          return -1;
        }
        rem_entry -= read_entry;

        if (rem_entry < 0) {
          return -1;
        }

        write_entry = write_file(file, rbuf, read_entry);
        if (write_entry != read_entry) {
          return -1;
        }
      }
      free(rbuf);
      return 0;
    }

    int r2s(std::string *str) {
      char *rbuf = (char *)malloc(RBUFSIZ);
      if (rbuf == nullptr) {
        return -1;
      }

      int64_t rem_entry = entry->uncompressed_size;
      int32_t read_entry;

      while (rem_entry > 0 and !canceled()) {
        read_entry = mz_zip_entry_read(parent, rbuf, RBUFSIZ);
        if (read_entry < 0) {
          return -1;
        }
        rem_entry -= read_entry;

        if (rem_entry < 0) {
          return -1;
        }

        str->append(rbuf, read_entry);
      }
      free(rbuf);
      return 0;
    }
  };

  Entry *current_entry = nullptr;
  bool cancel = false;

  Archive(Mystream *strm, int32_t mode = ZLIB_FILEFUNC_MODE_READ)
      : stream(strm) {
    zip = mz_zip_create();
    if (zip == nullptr) {
      throw Error();
    }
    mz_stream *s = strm->get_mz_stream();
    int res = mz_zip_open(zip, s, mode);
    if (res != MZ_OK) {
      throw Error();
    }
    res = mz_zip_get_number_entry(zip, &num_entries);
    if (res != MZ_OK) {
      throw Error();
    }
    res = mz_zip_goto_first_entry(zip);
    if (res != MZ_OK) {
      throw Error();
    }
    current_entry = 0;
  }

  int go_to_first_entry(Entry *e) {
    int res = mz_zip_goto_first_entry(zip);
    e->parent = zip;
    current_entry = e;
    e->archive = this;
    return res;
  }

  int32_t get_next_entry(Entry *e) {
    int res = mz_zip_goto_next_entry(zip);
    e->parent = zip;
    current_entry = e;
    e->archive = this;
    return res;
  }
};

struct ProgressWindow;

struct Extractor {
  struct Error : std::exception {
    std::string message;
    Error(std::string m = "An error ocurred while extracting") { message = m; }
    const char *what() const noexcept override { return message.c_str(); }
  };

  Archive *archive;
  std::string dir_path;
  std::string zip_root;
  std::string *zip;

  Extractor(Archive *a, std::string output_dir_path, std::string *zip_path) {
    if (!std::filesystem::is_directory(output_dir_path)) {
      throw Extractor::Error("Output folder isn't valid.");
    }
    dir_path = output_dir_path;
    zip = zip_path;
#ifdef _WIN32
    dir_path.append("\\");
#else
    dir_path.append("/");
#endif
    archive = a;
  }

  void extract_entry(Archive::Entry *entry,
                     bool (*excb)(const char *, size_t, bool, void *),
                     void *ctx) {
    int res;
    const char *name_ptr = entry->get_name();

    std::string name = std::string(dir_path.c_str(), dir_path.size());
    name.append(name_ptr);

    std::filesystem::path path(name);

    try {
      std::filesystem::create_directories(path.parent_path());
    } catch (std::filesystem::filesystem_error &e) {
      throw Extractor::Error("Failed to a create directory.");
    }

    // printf("extracting %s\n", name.c_str());

    bool exists = std::filesystem::exists(name);

    if (exists and !std::filesystem::is_directory(name)) {
      if (!excb(name.c_str(), name.length(), entry->is_dir(), ctx))
        return;
    }
    if (entry->is_dir()) {
      std::filesystem::create_directory(name);
    } else if (entry->is_symlink()) {
      if (exists) {
        if (!std::filesystem::remove(name)) {
          throw Extractor::Error("Failed to overwrite a symbolic link");
        }
      }
#ifdef _WIN32
      throw Extractor::Error("Failed to create link");
#else
      std::string link_data = "";
      res = entry->r2s(&link_data);
      if (res != 0) {
        throw Extractor::Error("Failed to read a symlink");
      }
      res = symlink(link_data.c_str(), name.c_str());
      if (res != 0) {
        throw Extractor::Error(
            std::error_code(errno, std::generic_category()).message());
      }
#endif
    } else {
      FILE *file = fopen(name.c_str(), "wb");
      if (file == nullptr)
        throw Extractor::Error("Failed to open a file");
      res = entry->write_to_file(file);
      if (res == -1) {
        throw Extractor::Error("Failed to write to file");
      }
      fclose(file);
    }
  }

  void extract(void (*cb)(bool, bool, std::string *, void *),
               bool (*excb)(const char *, size_t, bool, void *), void *ctx) {
    int res;
    bool first = true;
    Archive::Entry entry;
    res = archive->go_to_first_entry(&entry);
    while (res == MZ_OK and !archive->cancel) {
      res = entry.read_open();
      if (res != MZ_OK)
        throw Extractor::Error();
      if (first) {
        zip_root = std::string(entry.get_name());
        first = false;
      }
      extract_entry(&entry, excb, ctx);
      res = entry.read_close();
      cb(false, false, zip, ctx);
      res = archive->get_next_entry(&entry);
    }

    if (res == MZ_END_OF_LIST) {
      cb(true, false, zip, ctx);
    } else if (archive->cancel) {
      try {
        undo();
      } catch (std::exception &e) {
      }
      cb(true, true, zip, ctx);
    } else {
      if (res != MZ_OK) {
        throw Extractor::Error();
      }
    }
  }

  void undo() {
    if (zip_root.empty())
      return;
    // std::filesystem::remove_all(
    //     std::filesystem::path(dir_path.append(zip_root)));
  }

  void cancel() { archive->cancel = true; }
};

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QScrollArea>
#include <QTextBrowser>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtDBus/QtDBus>
#include <iterator>

struct DropBox : public QScrollArea {

  struct Item : QWidget {
    struct Btn : QPushButton {
      DropBox *db;

      Btn(Item *parent, DropBox *d) : QPushButton(parent) {
        db = d;
        QIcon icon = QIcon(":/icons/icons/delete.png");
        if (icon.isNull()) {
          setText("X");
        } else {
          setIcon(icon);
        }
        setFixedSize(30, 30);
      }

      void mousePressEvent(QMouseEvent *event) override {
        (void)event;
        Item *p = (Item *)parentWidget();
        db->remove_item(p);
      }
    };

    QHBoxLayout layout;
    QLabel label;
    Btn rmbtn;
    bool selected = false;

    Item(QWidget *parent, DropBox *d, const char *text)
        : QWidget(parent), layout(this), label(this), rmbtn(this, d) {
      setLayout(&layout);
      layout.addWidget(&rmbtn);
      layout.addWidget(&label);
      label.setText(QString(text));
      label.adjustSize();
      label.setAttribute(Qt::WA_TransparentForMouseEvents);
      rmbtn.setStyleSheet("border: 1px solid black; border-radius: 5px;");
      adjustSize();
    }

    void mousePressEvent(QMouseEvent *event) override {
      (void)event;
      selected = !selected;
    }

    std::string get_name() {
      auto txt = label.text();
      // printf("get name: %s\n", txt.toStdString().c_str());
      return txt.toStdString();
    }

    void redden() {
      label.setStyleSheet("color: red;");
      label.update();
      label.repaint();
    }
  };
  QLabel label;
  QWidget container;
  void (*on_drop)(QList<QUrl> *file_names, void *ctx);
  void *ctx;
  QVBoxLayout layout;
  QVBoxLayout back_layout;
  std::list<std::string> *part_paths;

  DropBox(QWidget *parent, std::list<std::string> *parts)
      : QScrollArea(parent), label(this), container(this), layout(&container),
        back_layout(this) {
    part_paths = parts;
    setAcceptDrops(true);
    back_layout.addWidget(&label);
    label.setText("Drop you ZIP file sequence here");
    label.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    label.setAlignment(Qt::AlignCenter);
    label.setAttribute(Qt::WA_TransparentForMouseEvents);
    label.stackUnder(&container);
    label.setStyleSheet("background-color: rgba(255, 255, 255, 0);"
                        "color: grey;"
                        "font-size: 30px;");
    container.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    container.setLayout(&layout);
    setWidget(&container);
    setStyleSheet("border: none; margin: 0px; padding: 0px;");
    setWidgetResizable(true);
  }

  void dragEnterEvent(QDragEnterEvent *event) override {
    if (event->mimeData()->hasUrls()) {
      event->acceptProposedAction();
    }
  }

  void dropEvent(QDropEvent *event) override {
    auto urls = event->mimeData()->urls();
    on_drop(&urls, ctx);
  }

  void onDrop(void (*cb)(QList<QUrl> *file_names, void *ctx), void *c) {
    on_drop = cb;
    this->ctx = c;
  }

  void add_item(const char *text) {
    Item *item = new Item(&container, this, text);
    layout.addWidget(item);
    item->raise();
    label.hide();
  }

  void remove_item(Item *p) {
    part_paths->remove_if(
        [p](std::string &s) { return !s.compare(p->get_name()); });
    layout.removeWidget(p);
    p->setParent(nullptr);
    p->deleteLater();

    if (part_paths->empty()) {
      label.show();
    }
  }

  void remove_by_name(std::string target_name) {
    for (auto child : container.children()) {
      QWidget *w = qobject_cast<QWidget *>(child);
      Item *i = reinterpret_cast<Item *>(w);
      if (w != nullptr) {
        std::string name = i->get_name();
        if (!name.compare(target_name)) {
          part_paths->remove_if(
              [i](std::string &s) { return !s.compare(i->get_name()); });
          layout.removeWidget(i);
          i->setParent(nullptr);
          i->deleteLater();
        }
      }
    }
  }

  void redden_by_name(std::string target_name) {
    for (auto child : container.children()) {
      QWidget *w = qobject_cast<QWidget *>(child);
      Item *i = reinterpret_cast<Item *>(w);
      if (w != nullptr) {
        std::string name = i->get_name();
        if (!name.compare(target_name)) {
          i->redden();
        }
      }
    }
  }
};

struct ProgressWindow : public QDialog {
  QVBoxLayout layout;
  QProgressBar progress_bar;
  QLabel label;
  QDialogButtonBox button_box;
  Extractor *x = nullptr;
  int pos;
  std::string *file_name = nullptr;

  ProgressWindow(QWidget *parent)
      : QDialog(parent), layout(this), progress_bar(this), label(this),
        button_box(QDialogButtonBox::Cancel, this) {
    layout.addWidget(&label);
    layout.addWidget(&progress_bar);
    setWindowTitle("Extracting");
    progress_bar.setRange(0, 100);
    progress_bar.setValue(0);
    layout.addWidget(&button_box);
    connect(&button_box, &QDialogButtonBox::rejected, this,
            &ProgressWindow::reject);
  }

  void reject() override {
    if (x != nullptr) {
      x->cancel();
    }
  }

  void setExtractor(Extractor *e, std::string *fname) {
    x = e;
    file_name = fname;
    if (file_name) {
      label.setText(QString::fromStdString(*file_name));
    }
  }

  void setRange(size_t low, size_t high) {
    progress_bar.setRange(low, high);
    pos = low;
  }

  void progress() {
    if (pos < progress_bar.maximum()) {
      pos += 1;
      progress_bar.setValue(pos);
      QCoreApplication::processEvents();
    }
  }

  void finish() {
    progress_bar.setValue(progress_bar.maximum());
    QCoreApplication::processEvents();
    hide();
    close();
    pos = progress_bar.maximum();
    label.setText("");
    file_name = nullptr;
  }

  void closeEvent(QCloseEvent *event) override {
    if (pos == progress_bar.maximum()) {
      event->accept();
    } else {
      event->ignore();
    }
  }

  void myShow(QPoint const &point) {
    auto my_size = sizeHint();
    move(point.x() - my_size.width(), point.y() - my_size.height());
    show();
    raise();
    activateWindow();
  }
};

struct LicenseDialog : public QDialog {
  QVBoxLayout layout;
  QTextBrowser tb;
  QDialogButtonBox button_box;

  const char *license_txt = R"(
<!-- HTML starts below -->
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <style>
    body {
      font-family: 'Segoe UI', sans-serif;
      font-size: 14px;
      color: #333;
      padding: 1em;
    }
    h1 {
      font-size: 18px;
      border-bottom: 1px solid #aaa;
      margin-bottom: 0.5em;
    }
    h2 {
      font-size: 16px;
      margin-top: 1.5em;
      margin-bottom: 0.3em;
    }
    ul {
      margin-left: 1.5em;
    }
    a {
      color: #007acc;
      text-decoration: none;
    }
    a:hover {
      text-decoration: underline;
    }
    .section {
      margin-bottom: 1em;
    }
  </style>
</head>
<body>
  <h3>End User Licence Agreement (EULA)</h3>

  <div class="section">
    <p>
      This End User Licence Agreement (the "Agreement") governs the use of the software product
      <strong>Zip Combiner</strong> (the "Software") provided by <strong>Studio Hanneman</strong> ("Vendor").
    </p>
    <p>
      By installing, copying, or otherwise using the Software, you ("Licensee") agree to be bound by
      the terms of this Agreement. If you do not agree, you must not install or use the Software.
    </p>
  </div>

  <h4>Licence</h4>
  <ul>
    <li>The Vendor grants the Licensee a non-exclusive, non-transferable licence to use the Software.</li>
    <li>"Software" includes the executable program and any related documentation or files provided.</li>
    <li>Title, copyright, intellectual property rights and distribution rights of the Software remain exclusively with the Vendor. This Agreement does not transfer ownership rights.</li>
    <li>The Software may be installed on only one computer at a time. One backup copy may be made solely for archival purposes.</li>
    <li>The Software may not be modified, reverse-engineered, decompiled, or distributed in any manner.</li>
    <li>Any violation of these terms will be considered a material breach of this Agreement.</li>
  </ul>

  <h4>Licence Fee</h4>
  <ul>
    <li>The licence fee for the Software will be the purchase price set by the Vendor at the time of transaction.</li>
    <li>Payment of this fee constitutes the full consideration for this Agreement.</li>
  </ul>

  <h4>Free or Trial Versions</h4>
  <ul>
    <li>If the Vendor provides the Software free of charge or as a trial version, the same terms of this Agreement apply.</li>
    <li>The Licensee acknowledges that free or trial versions may have limited features, functionality, or duration of use as determined by the Vendor.</li>
    <li>No warranty, support, or continued availability is guaranteed for free or trial versions, and the Vendor reserves the right to discontinue them at any time.</li>
  </ul>

  <h4>Updates and Upgrades</h4>
  <ul>
    <li>The Vendor may, at its sole discretion, provide updates, upgrades, or patches to the Software.</li>
    <li>Any such updates or upgrades are deemed part of the Software and are governed by this Agreement, unless accompanied by a separate licence agreement.</li>
    <li>The Licensee agrees that the Vendor has no obligation to provide updates or upgrades and that the availability, features, or performance of the Software may change as a result.</li>
    <li>The Vendor reserves the right to discontinue support for older versions of the Software upon release of an update.</li>
  </ul>

  <h4>Limitation of Liability</h4>
  <ul>
    <li>The Software is provided "as is".</li>
    <li>The Vendor’s liability will be limited to the amount paid for the Software (if any).</li>
    <li>The Vendor will not be liable for any indirect, incidental, or consequential damages, including but not limited to loss of data, revenue, profits, or business opportunities.</li>
    <li>The Vendor makes no warranties, expressed or implied, including but not limited to merchantability or fitness for a particular purpose.</li>
    <li>The Licensee acknowledges that all software may contain bugs or flaws.</li>
  </ul>

  <h4>Warranties and Representations</h4>
  <ul>
    <li>The Vendor represents that it is the copyright holder of the Software and has the right to grant this licence.</li>
  </ul>

  <h4>Acceptance</h4>
  <ul>
    <li>By installing or using the Software, the Licensee acknowledges acceptance of all terms and conditions in this Agreement.</li>
  </ul>

  <h4>User Support</h4>
  <ul>
    <li>No user support, maintenance, or updates are provided under this Agreement unless separately agreed in writing.</li>
  </ul>

  <h4>Term & Termination</h4>
  <ul>
    <li>This Agreement is effective upon the Licensee’s first use of the Software and is perpetual unless terminated.</li>
    <li>The Agreement and Licence will terminate automatically if the Licensee breaches any provision. Upon termination, the Licensee must delete all copies of the Software.</li>
  </ul>

  <h4>Force Majeure</h4>
  <ul>
    <li>The Vendor will not be held liable for delays or failures caused by events beyond its reasonable control, including natural disasters, war, or government actions.</li>
  </ul>

  <h4>Governing Law</h4>
  <ul>
    <li>This Agreement will be governed by and construed under the laws of the Province of Quebec, Canada.</li>
    <li>The parties submit to the jurisdiction of the courts of Quebec.</li>
  </ul>

  <h4>Notices</h4>
  <ul>
    <li>Notices or questions regarding this Agreement may be directed to the Vendor at:
      <a href="https://zipcombiner.com">https://zipcombiner.com</a>
    </li>
  </ul>
</body>
</html>
)";

  LicenseDialog(QWidget *parent)
      : QDialog(parent), layout(this), tb(this),
        button_box(QDialogButtonBox::NoButton, this) {
    setWindowTitle("License");
    tb.setOpenExternalLinks(true);
    tb.setHtml(license_txt);
    tb.setStyleSheet("QTextBrowser{margin: 0px; padding: 0px; border: none; }");
    // setStyleSheet("QWidget{margin: 0px; padding: 0px; border: none;}");
    // tb.document()->setDocumentMargin(0);
    resize(500, 400);
    layout.addWidget(&tb);
    layout.addWidget(&button_box);
    // connect(&button_box, &QDialogButtonBox::close, this,
    // &LicenseDialog::close);
  }

  void closeEvent(QCloseEvent *event) override { event->accept(); }

  void myShow(QPoint const &point) {
    move(point.x(), point.y());
    show();
    raise();
    activateWindow();
  }
};

struct AboutWindow : public QDialog {
  QVBoxLayout layout;
  QLabel label_logo;
  QLabel label_name;
  QLabel label_version;
  QLabel label_copy;
  QDialogButtonBox button_box;
  LicenseDialog ld;

  AboutWindow(QWidget *parent)
      : QDialog(parent), layout(this), label_logo(this), label_name(this),
        label_version(this), label_copy(this),
        button_box(QDialogButtonBox::Ok, this), ld(this) {
    layout.addWidget(&label_logo);
    layout.addWidget(&label_name);
    layout.addWidget(&label_version);
    layout.addWidget(&label_copy);
    setWindowTitle("About");

    QPixmap p = QPixmap(":/icons/icons/logo.png");
    label_logo.setPixmap(
        p.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label_logo.setFixedSize(50, 50);

    label_name.setText("Zip Combiner");
    label_name.setStyleSheet("font-weight: bold;");
    label_version.setText("Version 1.3");
    label_copy.setText("Copyright 2025 Studio Hanneman");

    layout.addWidget(&button_box);
    button_box.button(QDialogButtonBox::Ok)->setText("License");
    button_box.button(QDialogButtonBox::Ok)->setIcon(QIcon());
    AboutWindow::connect(&button_box, &QDialogButtonBox::accepted, this,
                         &AboutWindow::open_license);
  }

  void open_license() { ld.myShow(geometry().center()); }

  void closeEvent(QCloseEvent *event) override { event->accept(); }

  void myShow(QPoint const &point) {
    move(point.x(), point.y());
    show();
    raise();
    activateWindow();
  }
};

struct ExistDialog : public QMessageBox {
  QCheckBox dont_ask;
  bool overw = false;
  ExistDialog(QWidget *parent) : QMessageBox(parent), dont_ask(this) {
    dont_ask.setText("Don't ask again.");
    setCheckBox(&dont_ask);
    setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  }

  bool dontAsk() { return dont_ask.isChecked(); }

  void alwaysAsk() {
    dont_ask.setCheckState(Qt::Unchecked);
    overw = false;
  }

  int ask(QString message) {
    setText(message);
    return exec();
  }
};

struct DoneDialog : public QMessageBox {
  QCheckBox dont_ask;
  bool del = false;
  DoneDialog(QWidget *parent) : QMessageBox(parent), dont_ask(this) {
    dont_ask.setText("Don't ask again.");
    setCheckBox(&dont_ask);
    setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  }

  bool dontAsk() { return dont_ask.isChecked(); }

  void alwaysAsk() {
    dont_ask.setCheckState(Qt::Unchecked);
    del = false;
  }

  int ask(QString message) {
    setText(message);
    return exec();
  }
};

struct FailDialog : public QMessageBox {
  QCheckBox dont_ask;
  bool del = false;
  FailDialog(QWidget *parent) : QMessageBox(parent), dont_ask(this) {
    dont_ask.setText("Don't ask again.");
    setCheckBox(&dont_ask);
    setIcon(QMessageBox::Warning);
    setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  }

  bool dontAsk() { return dont_ask.isChecked(); }

  void alwaysAsk() {
    dont_ask.setCheckState(Qt::Unchecked);
    del = false;
  }

  int ask(QString message) {
    setText(message);
    return exec();
  }
};

struct App : public QApplication {
  QMainWindow window;
  QWidget main_widget;
  QMenu file_menu;
  QAction action_file_open;
  QAction action_extract;
  QAction action_license;
  DropBox drop_box;
  QToolBar toolbar;
  QFileDialog file_dialog;
  ProgressWindow progress_window;
  ExistDialog exist_dialog;
  DoneDialog done_dialog;
  FailDialog fail_dialog;
  QVBoxLayout main_layout;
  QMenu about_menu;
  AboutWindow about_window;

  int errors = 0;
  int not_errors = 0;

  std::list<std::string> part_paths;
  bool canceled = false;

  App(int argc, char *argv[])
      : QApplication(argc, argv), file_menu("File"),
        action_file_open("Add"), action_extract("Extract"),
        action_license("About"), drop_box(&main_widget, &part_paths),
        toolbar(&window), file_dialog(&main_widget),
        progress_window(&main_widget), exist_dialog(&main_widget),
        done_dialog(&main_widget), fail_dialog(&main_widget),
        about_menu("About", &window), about_window(&main_widget) {
    window.resize(600, 400);
    window.setCentralWidget(&main_widget);

    //main_widget.resize(window.size());
    main_widget.setLayout(&main_layout);
    main_layout.addWidget(&drop_box);

    action_file_open.setIcon(QIcon(":/icons/icons/open.png"));
    action_extract.setIcon(QIcon(":/icons/icons/extract.png"));

    drop_box.onDrop(
        [](QList<QUrl> *urls, void *ctx) {
          App *a = (App *)ctx;
          for (int64_t i = 0; i < urls->size(); i++) {
            auto path = urls->at(i).toLocalFile().toStdString();
            if (a->find_file(path))
              continue;
            a->part_paths.push_back(path.c_str());
            a->drop_box.add_item(path.c_str());
          }

          // a->part_paths.sort();
          QCoreApplication::processEvents();
        },
        this);

    QMainWindow::connect(&action_file_open, &QAction::triggered,
                         [this]() { open_files(); });

    QMainWindow::connect(&action_extract, &QAction::triggered, [this]() {
      if (part_paths.empty())
        return;
      std::string out_dir = open_out_dir();
      if (!out_dir.empty()) {
        printf(" len %lu\n", part_paths.size());
        extract(this, out_dir);
      }
    });

    QMainWindow::connect(&action_license, &QAction::triggered, [this]() {
      about_window.myShow(window.geometry().topLeft());
    });

    toolbar.addAction(&action_file_open);
    toolbar.addAction(&action_extract);

    about_menu.addAction(&action_license);
    window.menuBar()->addMenu(&about_menu);
    // about_menu.addAction(&action_license);

    window.addToolBar(&toolbar);

    window.setWindowTitle("Zip Combiner");
    window.show();

    // qDebug("====== APP LAUNCHED =====\n");
  }

  void finishExtraction() {
    progress_window.finish();
    progress_window.setExtractor(nullptr, nullptr);
  }

  void deleteZIP(const char *file_name, size_t file_name_len) {
    not_errors += 1;
    if (!done_dialog.dontAsk()) {
      std::string qs("");
      qs.append(file_name, file_name_len);
      qs.append(" extracted. Do you want to delete it?");
      if (done_dialog.ask(QString::fromStdString(qs)) == QMessageBox::Yes) {
        done_dialog.del = true;
      } else {
        done_dialog.del = false;
      }
    }

    if (done_dialog.del) {
      try {
        std::filesystem::remove(file_name);
      } catch (std::exception &e) {
      }
    }
  }

  void fail(std::string zip_file_name, const char *error_message) {
    finishExtraction();
    errors += 1;
    if (!fail_dialog.dontAsk() and not_errors + errors < part_paths.size()) {
      std::string qs("An error occured while extracting ");
      qs.append(zip_file_name);
      qs.append(": ");
      qs.append(error_message);
      qs.append(". Do you want to continue for other zip files?");
      if (fail_dialog.ask(QString::fromStdString(qs)) == QMessageBox::Yes) {
        fail_dialog.del = true;
      } else {
        fail_dialog.del = false;
      }
    }

    drop_box.redden_by_name(zip_file_name);
    canceled = done_dialog.del;
  }

  static void extract(App *app, std::string od) {
    app->fail_dialog.alwaysAsk();
    app->exist_dialog.alwaysAsk();
    app->done_dialog.alwaysAsk();
    std::thread([app, od = std::move(od)]() {
      auto part_paths_copy = app->part_paths;
      for (auto parts_i : part_paths_copy) {
        if (app->canceled)
          break;
        try {
          Mystream z(&parts_i);
          Archive a(&z);
          Extractor x(&a, od, &parts_i);

          QMetaObject::invokeMethod(
              (QObject *)app,
              [&x, app, a = std::move(a), &parts_i]() {
                app->progress_window.setRange(0, a.num_entries);
                app->progress_window.myShow(app->window.geometry().center());
                app->progress_window.setExtractor(&x, &parts_i);
              },
              Qt::BlockingQueuedConnection);

          x.extract(
              [](bool done, bool cancel, std::string *zip, void *ctx) {
                QMetaObject::invokeMethod(
                    (QObject *)ctx,
                    [ctx, done, cancel, zip]() {
                      App *a = (App *)ctx;
                      a->canceled = cancel;
                      a->progress_window.progress();
                      if (done) {
                        a->finishExtraction();
                        assert(zip != nullptr);
                        if (!cancel) {
                          a->deleteZIP(zip->c_str(), zip->length());
                          a->drop_box.remove_by_name(*zip);
                        }
                      }
                    },
                    Qt::BlockingQueuedConnection);
              },
              [](const char *file_name, size_t file_name_len, bool is_dir,
                 void *ctx) {
                int skip;
                QMetaObject::invokeMethod(
                    (QObject *)ctx,
                    [&skip, file_name, file_name_len, is_dir, ctx]() {
                      App *a = (App *)ctx;
                      if (a->exist_dialog.dontAsk()) {
                        skip = a->exist_dialog.overw ? QMessageBox::Yes
                                                     : QMessageBox::No;
                        return;
                      }
                      std::string qs("");
                      if (is_dir) {
                        qs.append("Folder ");
                      } else {
                        qs.append("File ");
                      }
                      qs.append(file_name, file_name_len);
                      qs.append(" exists. Do you want to overwrite it?");
                      skip = a->exist_dialog.ask(QString::fromStdString(qs));
                      a->exist_dialog.overw = skip == QMessageBox::Yes;
                    },
                    Qt::BlockingQueuedConnection);
                return skip == QMessageBox::Yes;
              },
              app);
        } catch (std::exception &e) {
          QMetaObject::invokeMethod((QObject *)app,
                                    [app, &e, &parts_i]() {
                                      app->canceled = true;
                                      app->fail(parts_i, e.what());
                                    },
                                    Qt::BlockingQueuedConnection);
        }
      }
      if (!app->canceled) {
        QMetaObject::invokeMethod(
            (QObject *)app,
            [app]() {
              App *a = (App *)app;

              QMessageBox::information(
                  &a->main_widget, "Done",
                  QString::asprintf("Extraction completed with %d error(s).",
                                    a->errors));
            },
            Qt::BlockingQueuedConnection);
      }
    }).detach();
  }

  auto get_file_at(size_t idx) {
    auto it = part_paths.begin();
    std::advance(it, idx);
    return it;
  }

  bool find_file(std::string &name) {
    return std::find(part_paths.begin(), part_paths.end(), name) !=
           part_paths.end();
  }

  void open_files() {
    file_dialog.setFileMode(QFileDialog::ExistingFiles);
    file_dialog.setViewMode(QFileDialog::Detail);

    if (file_dialog.exec()) {
      auto files = file_dialog.selectedFiles();
      for (int64_t i = 0; i < files.size(); i++) {
        auto path = files.at(i).toStdString();
        if (find_file(path))
          continue;
        part_paths.push_back(path.c_str());
        drop_box.add_item(path.c_str());
      }
    }

    // part_paths.sort();
  }

  std::string open_out_dir() {
    auto dir = QFileDialog::getExistingDirectory(
        nullptr, "Select a folder to extract to", QDir::homePath(),
        QFileDialog::ShowDirsOnly);
    return dir.toStdString();
  }
};

void dbus_dummy() {
#ifdef __APPLE__
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    qFatal("Cannot connect to the D-Bus session bus.");
  }
#endif
}

int main(int argc, char *argv[]) {
  qInitResources();
  // qDebug("====== APP STARTING =====\n");
  QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);
  App *app = new App(argc, argv);
  return app->exec();
}
