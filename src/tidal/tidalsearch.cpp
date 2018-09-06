/*
 * Strawberry Music Player
 * This code was part of Clementine (GlobalSearch)
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <algorithm>

#include <QtGlobal>
#include <QObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QStringBuilder>
#include <QUrl>
#include <QImage>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QTimerEvent>
#include <QSettings>

#include "core/application.h"
#include "core/logging.h"
#include "core/closure.h"
#include "core/iconloader.h"
#include "covermanager/albumcoverloader.h"
#include "internet/internetsongmimedata.h"
#include "playlist/songmimedata.h"
#include "tidalsearch.h"
#include "tidalservice.h"
#include "settings/tidalsettingspage.h"

const int TidalSearch::kDelayedSearchTimeoutMs = 200;
const int TidalSearch::kMaxResultsPerEmission = 1000;
const int TidalSearch::kArtHeight = 32;

TidalSearch::TidalSearch(Application *app, QObject *parent)
    : QObject(parent),
      app_(app),
      service_(app->internet_model()->Service<TidalService>()),
      name_("Tidal"),
      id_("tidal"),
      icon_(IconLoader::Load("tidal")),
      searches_next_id_(1),
      art_searches_next_id_(1) {

  cover_loader_options_.desired_height_ = kArtHeight;
  cover_loader_options_.pad_output_image_ = true;
  cover_loader_options_.scale_output_image_ = true;

  connect(app_->album_cover_loader(), SIGNAL(ImageLoaded(quint64, QImage)), SLOT(AlbumArtLoaded(quint64, QImage)));
  connect(this, SIGNAL(SearchAsyncSig(int, QString, TidalSettingsPage::SearchBy)), this, SLOT(DoSearchAsync(int, QString, TidalSettingsPage::SearchBy)));
  connect(this, SIGNAL(ResultsAvailable(int, TidalSearch::ResultList)), SLOT(ResultsAvailableSlot(int, TidalSearch::ResultList)));
  connect(this, SIGNAL(ArtLoaded(int, QImage)), SLOT(ArtLoadedSlot(int, QImage)));
  connect(service_, SIGNAL(SearchResults(int, SongList)), SLOT(SearchDone(int, SongList)));
  connect(service_, SIGNAL(SearchError(int, QString)), SLOT(HandleError(int, QString)));

  icon_as_image_ = QImage(icon_.pixmap(48, 48).toImage());

}

TidalSearch::~TidalSearch() {}

QStringList TidalSearch::TokenizeQuery(const QString &query) {

  QStringList tokens(query.split(QRegExp("\\s+")));

  for (QStringList::iterator it = tokens.begin(); it != tokens.end(); ++it) {
    (*it).remove('(');
    (*it).remove(')');
    (*it).remove('"');

    const int colon = (*it).indexOf(":");
    if (colon != -1) {
      (*it).remove(0, colon + 1);
    }
  }

  return tokens;

}

bool TidalSearch::Matches(const QStringList &tokens, const QString &string) {

  for (const QString &token : tokens) {
    if (!string.contains(token, Qt::CaseInsensitive)) {
      return false;
    }
  }

  return true;

}

int TidalSearch::SearchAsync(const QString &query, TidalSettingsPage::SearchBy searchby) {

  const int id = searches_next_id_++;

  emit SearchAsyncSig(id, query, searchby);

  return id;

}

void TidalSearch::SearchAsync(int id, const QString &query, TidalSettingsPage::SearchBy searchby) {

  const int service_id = service_->Search(query, searchby);
  pending_searches_[service_id] = PendingState(id, TokenizeQuery(query));

}

void TidalSearch::DoSearchAsync(int id, const QString &query, TidalSettingsPage::SearchBy searchby) {

  int timer_id = startTimer(kDelayedSearchTimeoutMs);
  delayed_searches_[timer_id].id_ = id;
  delayed_searches_[timer_id].query_ = query;
  delayed_searches_[timer_id].searchby_ = searchby;

}

void TidalSearch::SearchDone(int service_id, const SongList &songs) {

  // Map back to the original id.
  const PendingState state = pending_searches_.take(service_id);
  const int search_id = state.orig_id_;

  ResultList ret;
  for (const Song &song : songs) {
    Result result;
    result.metadata_ = song;
    ret << result;
  }

  emit ResultsAvailable(search_id, ret);
  MaybeSearchFinished(search_id);

}

void TidalSearch::HandleError(const int id, const QString error) {

  emit SearchError(id, error);

}

void TidalSearch::MaybeSearchFinished(int id) {

  if (pending_searches_.keys(PendingState(id, QStringList())).isEmpty()) {
    emit SearchFinished(id);
  }

}

void TidalSearch::CancelSearch(int id) {
  QMap<int, DelayedSearch>::iterator it;
  for (it = delayed_searches_.begin(); it != delayed_searches_.end(); ++it) {
    if (it.value().id_ == id) {
      killTimer(it.key());
      delayed_searches_.erase(it);
      return;
    }
  }
}

void TidalSearch::timerEvent(QTimerEvent *e) {
  QMap<int, DelayedSearch>::iterator it = delayed_searches_.find(e->timerId());
  if (it != delayed_searches_.end()) {
    SearchAsync(it.value().id_, it.value().query_, it.value().searchby_);
    delayed_searches_.erase(it);
    return;
  }

  QObject::timerEvent(e);
}

void TidalSearch::ResultsAvailableSlot(int id, TidalSearch::ResultList results) {

  if (results.isEmpty()) return;

  // Limit the number of results that are used from each emission.
  if (results.count() > kMaxResultsPerEmission) {
    TidalSearch::ResultList::iterator begin = results.begin();
    std::advance(begin, kMaxResultsPerEmission);
    results.erase(begin, results.end());
  }

  // Load cached pixmaps into the results
  for (TidalSearch::ResultList::iterator it = results.begin(); it != results.end(); ++it) {
    it->pixmap_cache_key_ = PixmapCacheKey(*it);
  }

  emit AddResults(id, results);

}

QString TidalSearch::PixmapCacheKey(const TidalSearch::Result &result) const {
  return "tidal:" % result.metadata_.url().toString();
}

bool TidalSearch::FindCachedPixmap(const TidalSearch::Result &result, QPixmap *pixmap) const {
  return pixmap_cache_.find(result.pixmap_cache_key_, pixmap);
}

int TidalSearch::LoadArtAsync(const TidalSearch::Result &result) {

  const int id = art_searches_next_id_++;

  pending_art_searches_[id] = result.pixmap_cache_key_;

  quint64 loader_id = app_->album_cover_loader()->LoadImageAsync(cover_loader_options_, result.metadata_);
  cover_loader_tasks_[loader_id] = id;

  return id;

}

void TidalSearch::ArtLoadedSlot(int id, const QImage &image) {
  HandleLoadedArt(id, image);
}

void TidalSearch::AlbumArtLoaded(quint64 id, const QImage &image) {

  if (!cover_loader_tasks_.contains(id)) return;
  int orig_id = cover_loader_tasks_.take(id);

  HandleLoadedArt(orig_id, image);

}

void TidalSearch::HandleLoadedArt(int id, const QImage &image) {

  const QString key = pending_art_searches_.take(id);

  QPixmap pixmap = QPixmap::fromImage(image);
  pixmap_cache_.insert(key, pixmap);

  emit ArtLoaded(id, pixmap);

}

QImage TidalSearch::ScaleAndPad(const QImage &image) {
    
  if (image.isNull()) return QImage();

  const QSize target_size = QSize(kArtHeight, kArtHeight);

  if (image.size() == target_size) return image;

  // Scale the image down
  QImage copy;
  copy = image.scaled(target_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

  // Pad the image to kHeight x kHeight
  if (copy.size() == target_size) return copy;

  QImage padded_image(kArtHeight, kArtHeight, QImage::Format_ARGB32);
  padded_image.fill(0);

  QPainter p(&padded_image);
  p.drawImage((kArtHeight - copy.width()) / 2, (kArtHeight - copy.height()) / 2, copy);
  p.end();

  return padded_image;

}

MimeData *TidalSearch::LoadTracks(const ResultList &results) {

  if (results.isEmpty()) {
    return nullptr;
  }

  ResultList results_copy;
  for (const Result &result : results) {
    results_copy << result;
  }

  SongList songs;
  for (const Result &result : results) {
    songs << result.metadata_;
  }

  InternetSongMimeData *internet_song_mime_data = new InternetSongMimeData(service_);
  internet_song_mime_data->songs = songs;
  MimeData *mime_data = internet_song_mime_data;

  QList<QUrl> urls;
  for (const Result &result : results) {
    urls << result.metadata_.url();
  }
  mime_data->setUrls(urls);

  return mime_data;

}