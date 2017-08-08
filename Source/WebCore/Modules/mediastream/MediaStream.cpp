/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2011, 2012, 2015 Ericsson AB. All rights reserved.
 * Copyright (C) 2013-2016 Apple Inc. All rights reserved.
 * Copyright (C) 2013 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "MediaStream.h"

#if ENABLE(MEDIA_STREAM)

#include "Document.h"
#include "Event.h"
#include "EventNames.h"
#include "Frame.h"
#include "Logging.h"
#include "MediaStreamRegistry.h"
#include "MediaStreamTrackEvent.h"
#include "NetworkingContext.h"
#include "Page.h"
#include "RealtimeMediaSource.h"
#include "URL.h"

namespace WebCore {

Ref<MediaStream> MediaStream::create(ScriptExecutionContext& context)
{
    return MediaStream::create(context, MediaStreamPrivate::create(Vector<RefPtr<MediaStreamTrackPrivate>>()));
}

Ref<MediaStream> MediaStream::create(ScriptExecutionContext& context, MediaStream& stream)
{
    return adoptRef(*new MediaStream(context, stream.getTracks()));
}

Ref<MediaStream> MediaStream::create(ScriptExecutionContext& context, const MediaStreamTrackVector& tracks)
{
    return adoptRef(*new MediaStream(context, tracks));
}

Ref<MediaStream> MediaStream::create(ScriptExecutionContext& context, Ref<MediaStreamPrivate>&& streamPrivate)
{
    return adoptRef(*new MediaStream(context, WTFMove(streamPrivate)));
}

static inline MediaStreamTrackPrivateVector createTrackPrivateVector(const MediaStreamTrackVector& tracks)
{
    MediaStreamTrackPrivateVector trackPrivates;
    trackPrivates.reserveCapacity(tracks.size());
    for (auto& track : tracks)
        trackPrivates.append(&track->privateTrack());
    return trackPrivates;
}

MediaStream::MediaStream(ScriptExecutionContext& context, const MediaStreamTrackVector& tracks)
    : ContextDestructionObserver(&context)
    , m_private(MediaStreamPrivate::create(createTrackPrivateVector(tracks)))
    , m_activityEventTimer(*this, &MediaStream::activityEventTimerFired)
    , m_mediaSession(PlatformMediaSession::create(*this))
{
    // This constructor preserves MediaStreamTrack instances and must be used by calls originating
    // from the JavaScript MediaStream constructor.

    for (auto& track : tracks) {
        track->addObserver(*this);
        m_trackSet.add(track->id(), track);
    }

    setIsActive(m_private->active());
    m_private->addObserver(*this);
    MediaStreamRegistry::shared().registerStream(*this);
    document()->addAudioProducer(this);
}

MediaStream::MediaStream(ScriptExecutionContext& context, Ref<MediaStreamPrivate>&& streamPrivate)
    : ContextDestructionObserver(&context)
    , m_private(WTFMove(streamPrivate))
    , m_activityEventTimer(*this, &MediaStream::activityEventTimerFired)
    , m_mediaSession(PlatformMediaSession::create(*this))
{
    setIsActive(m_private->active());
    if (document()->page() && document()->page()->isMediaCaptureMuted())
        m_private->setCaptureTracksMuted(true);
    m_private->addObserver(*this);
    MediaStreamRegistry::shared().registerStream(*this);

    for (auto& trackPrivate : m_private->tracks()) {
        auto track = MediaStreamTrack::create(context, *trackPrivate);
        track->addObserver(*this);
        m_trackSet.add(track->id(), WTFMove(track));
    }
    document()->addAudioProducer(this);
}

MediaStream::~MediaStream()
{
    // Set isActive to false immediately so any callbacks triggered by shutting down, e.g.
    // mediaState(), are short circuited.
    m_isActive = false;
    MediaStreamRegistry::shared().unregisterStream(*this);
    m_private->removeObserver(*this);
    for (auto& track : m_trackSet.values())
        track->removeObserver(*this);
    if (Document* document = this->document()) {
        document->removeAudioProducer(this);
        if (m_isWaitingUntilMediaCanStart)
            document->removeMediaCanStartListener(this);
    }
}

RefPtr<MediaStream> MediaStream::clone()
{
    MediaStreamTrackVector clonedTracks;
    clonedTracks.reserveInitialCapacity(m_trackSet.size());

    for (auto& track : m_trackSet.values())
        clonedTracks.uncheckedAppend(track->clone());

    return MediaStream::create(*scriptExecutionContext(), clonedTracks);
}

void MediaStream::addTrack(MediaStreamTrack& track)
{
    if (!internalAddTrack(track, StreamModifier::DomAPI))
        return;

    for (auto& observer : m_observers)
        observer->didAddOrRemoveTrack();
}

void MediaStream::removeTrack(MediaStreamTrack& track)
{
    if (!internalRemoveTrack(track.id(), StreamModifier::DomAPI))
        return;

    for (auto& observer : m_observers)
        observer->didAddOrRemoveTrack();
}

MediaStreamTrack* MediaStream::getTrackById(String id)
{
    auto it = m_trackSet.find(id);
    if (it != m_trackSet.end())
        return it->value.get();

    return nullptr;
}

MediaStreamTrackVector MediaStream::getAudioTracks() const
{
    return trackVectorForType(RealtimeMediaSource::Type::Audio);
}

MediaStreamTrackVector MediaStream::getVideoTracks() const
{
    return trackVectorForType(RealtimeMediaSource::Type::Video);
}

MediaStreamTrackVector MediaStream::getTracks() const
{
    MediaStreamTrackVector tracks;
    tracks.reserveCapacity(m_trackSet.size());
    copyValuesToVector(m_trackSet, tracks);

    return tracks;
}

void MediaStream::contextDestroyed()
{
    ContextDestructionObserver::contextDestroyed();
}

void MediaStream::trackDidEnd()
{
    m_private->updateActiveState(MediaStreamPrivate::NotifyClientOption::Notify);
}

void MediaStream::activeStatusChanged()
{
    // Schedule the active state change and event dispatch since this callback may be called
    // synchronously from the DOM API (e.g. as a result of addTrack()).
    scheduleActiveStateChange();
}

void MediaStream::didAddTrack(MediaStreamTrackPrivate& trackPrivate)
{
    ScriptExecutionContext* context = scriptExecutionContext();
    if (!context)
        return;

    if (!getTrackById(trackPrivate.id()))
        internalAddTrack(MediaStreamTrack::create(*context, trackPrivate), StreamModifier::Platform);
}

void MediaStream::didRemoveTrack(MediaStreamTrackPrivate& trackPrivate)
{
    internalRemoveTrack(trackPrivate.id(), StreamModifier::Platform);
}

void MediaStream::addTrackFromPlatform(Ref<MediaStreamTrack>&& track)
{
    m_private->addTrack(&track->privateTrack(), MediaStreamPrivate::NotifyClientOption::Notify);
    internalAddTrack(WTFMove(track), StreamModifier::Platform);
}

bool MediaStream::internalAddTrack(Ref<MediaStreamTrack>&& trackToAdd, StreamModifier streamModifier)
{
    auto result = m_trackSet.add(trackToAdd->id(), WTFMove(trackToAdd));
    if (!result.isNewEntry)
        return false;

    ASSERT(result.iterator->value);
    auto& track = *result.iterator->value;
    track.addObserver(*this);

    if (streamModifier == StreamModifier::DomAPI)
        m_private->addTrack(&track.privateTrack(), MediaStreamPrivate::NotifyClientOption::DontNotify);
    else
        dispatchEvent(MediaStreamTrackEvent::create(eventNames().addtrackEvent, false, false, &track));

    return true;
}

bool MediaStream::internalRemoveTrack(const String& trackId, StreamModifier streamModifier)
{
    auto track = m_trackSet.take(trackId);
    if (!track)
        return false;

    track->removeObserver(*this);

    if (streamModifier == StreamModifier::DomAPI)
        m_private->removeTrack(track->privateTrack(), MediaStreamPrivate::NotifyClientOption::DontNotify);
    else
        dispatchEvent(MediaStreamTrackEvent::create(eventNames().removetrackEvent, false, false, WTFMove(track)));

    return true;
}

void MediaStream::setIsActive(bool active)
{
    if (m_isActive == active)
        return;

    m_isActive = active;
    statusDidChange();
}

void MediaStream::mediaCanStart(Document& document)
{
    ASSERT_UNUSED(document, &document == this->document());
    ASSERT(m_isWaitingUntilMediaCanStart);
    if (m_isWaitingUntilMediaCanStart) {
        m_isWaitingUntilMediaCanStart = false;
        startProducingData();
    }
}

void MediaStream::startProducingData()
{
    Document* document = this->document();
    if (!document || !document->page())
        return;

    // If we can't start a load right away, start it later.
    if (!document->page()->canStartMedia()) {
        LOG(Media, "MediaStream::startProducingData(%p) - not allowed to start in background, waiting", this);
        if (m_isWaitingUntilMediaCanStart)
            return;

        m_isWaitingUntilMediaCanStart = true;
        document->addMediaCanStartListener(this);
        return;
    }

    if (m_isProducingData)
        return;
    m_isProducingData = true;

    m_mediaSession->canProduceAudioChanged();

    m_private->startProducingData();
}

void MediaStream::stopProducingData()
{
    if (!m_isProducingData)
        return;
    m_isProducingData = false;

    m_mediaSession->canProduceAudioChanged();

    m_private->stopProducingData();
}

void MediaStream::endCaptureTracks()
{
    for (auto& track : m_trackSet.values()) {
        if (track->isCaptureTrack())
            track->stopTrack(MediaStreamTrack::StopMode::PostEvent);
    }
}

void MediaStream::pageMutedStateDidChange()
{
    if (!m_isActive)
        return;

    Document* document = this->document();
    if (!document)
        return;

    m_private->setCaptureTracksMuted(document->page()->isMediaCaptureMuted());
}

MediaProducer::MediaStateFlags MediaStream::mediaState() const
{
    MediaStateFlags state = IsNotPlaying;

    if (!m_isActive)
        return state;

    if (m_private->hasAudio()) {
        state |= HasAudioOrVideo;
        if (m_private->hasCaptureAudioSource()) {
            if (m_private->muted())
                state |= HasMutedAudioCaptureDevice;
            else if (m_isProducingData && m_private->isProducingData())
                state |= HasActiveAudioCaptureDevice;
        }
    }

    if (m_private->hasVideo()) {
        state |= HasAudioOrVideo;
        if (m_private->hasCaptureVideoSource()) {
            if (m_private->muted())
                state |= HasMutedVideoCaptureDevice;
            else if (m_isProducingData && m_private->isProducingData())
                state |= HasActiveVideoCaptureDevice;
        }
    }

    return state;
}

void MediaStream::statusDidChange()
{
    m_mediaSession->canProduceAudioChanged();

    if (Document* document = this->document()) {
        if (m_isActive)
            document->setHasActiveMediaStreamTrack();
        document->updateIsPlayingMedia();
    }
}

void MediaStream::characteristicsChanged()
{
    bool muted = m_private->muted();
    if (m_isMuted != muted) {
        m_isMuted = muted;
        statusDidChange();
    }
}

void MediaStream::scheduleActiveStateChange()
{
    bool active = false;
    for (auto& track : m_trackSet.values()) {
        if (!track->ended()) {
            active = true;
            break;
        }
    }

    if (m_isActive == active)
        return;

    setIsActive(active);

    const AtomicString& eventName = m_isActive ? eventNames().inactiveEvent : eventNames().activeEvent;
    m_scheduledActivityEvents.append(Event::create(eventName, false, false));

    if (!m_activityEventTimer.isActive())
        m_activityEventTimer.startOneShot(0_s);
}

void MediaStream::activityEventTimerFired()
{
    Vector<Ref<Event>> events;
    events.swap(m_scheduledActivityEvents);

    for (auto& event : events)
        dispatchEvent(event);
}

URLRegistry& MediaStream::registry() const
{
    return MediaStreamRegistry::shared();
}

MediaStreamTrackVector MediaStream::trackVectorForType(RealtimeMediaSource::Type filterType) const
{
    MediaStreamTrackVector tracks;
    for (auto& track : m_trackSet.values()) {
        if (track->source().type() == filterType)
            tracks.append(track);
    }

    return tracks;
}

void MediaStream::addObserver(MediaStream::Observer* observer)
{
    if (m_observers.find(observer) == notFound)
        m_observers.append(observer);
}

void MediaStream::removeObserver(MediaStream::Observer* observer)
{
    size_t pos = m_observers.find(observer);
    if (pos != notFound)
        m_observers.remove(pos);
}

Document* MediaStream::document() const
{
    return downcast<Document>(scriptExecutionContext());
}

PlatformMediaSession::MediaType MediaStream::mediaType() const
{
    // We only need to override the type when capturing audio, HTMLMediaElement and/or WebAudio
    // will do the right thing when a stream is attached to a media element or an audio context.
    if (m_private->hasAudio() && m_isProducingData && m_private->hasCaptureAudioSource())
        return PlatformMediaSession::MediaStreamCapturingAudio;

    return PlatformMediaSession::None;
}

PlatformMediaSession::MediaType MediaStream::presentationType() const
{
    return mediaType();
}

PlatformMediaSession::CharacteristicsFlags MediaStream::characteristics() const
{
    PlatformMediaSession::CharacteristicsFlags state = PlatformMediaSession::HasNothing;

    if (!m_isProducingData)
        return state;

    if (m_private->hasAudio())
        state |= PlatformMediaSession::HasAudio;

    if (m_private->hasVideo())
        state |= PlatformMediaSession::HasVideo;

    return state;
}

void MediaStream::mayResumePlayback(bool)
{
    // FIXME: should a media stream pay attention to this directly, or only when attached to a media element?
}

void MediaStream::suspendPlayback()
{
    // FIXME: should a media stream pay attention to this directly, or only when attached to a media element?
}

String MediaStream::sourceApplicationIdentifier() const
{
    Document* document = this->document();
    if (document && document->frame()) {
        if (NetworkingContext* networkingContext = document->frame()->loader().networkingContext())
            return networkingContext->sourceApplicationIdentifier();
    }

    return emptyString();
}

bool MediaStream::canProduceAudio() const
{
    return !muted() && active() && m_private->hasAudio() && m_isProducingData;
}

bool MediaStream::processingUserGestureForMedia() const
{
    return document() ? document()->processingUserGestureForMedia() : false;
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)