#include "session_lock.hpp"

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qobject.h>
#include <qqmlcomponent.h>
#include <qqmlengine.h>
#include <qqmllist.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qscreen.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../core/qmlscreen.hpp"
#include "../core/reload.hpp"
#include "session_lock/session_lock.hpp"

void WlSessionLock::onReload(QObject* oldInstance) {
	auto* old = qobject_cast<WlSessionLock*>(oldInstance);

	if (old != nullptr) {
		QObject::disconnect(old->manager, nullptr, old, nullptr);
		this->manager = old->manager;
		this->manager->setParent(this);
	} else {
		this->manager = new SessionLockManager(this);
	}

	// clang-format off
	QObject::connect(this->manager, &SessionLockManager::locked, this, &WlSessionLock::secureStateChanged);
	QObject::connect(this->manager, &SessionLockManager::unlocked, this, &WlSessionLock::secureStateChanged);

	QObject::connect(this->manager, &SessionLockManager::unlocked, this, &WlSessionLock::unlock);

	auto* app = QCoreApplication::instance();
	auto* guiApp = qobject_cast<QGuiApplication*>(app);

	if (guiApp != nullptr) {
		QObject::connect(guiApp, &QGuiApplication::primaryScreenChanged, this, &WlSessionLock::onScreensChanged);
		QObject::connect(guiApp, &QGuiApplication::screenAdded, this, &WlSessionLock::onScreensChanged);
		QObject::connect(guiApp, &QGuiApplication::screenRemoved, this, &WlSessionLock::onScreensChanged);
	}
	// clang-format on

	if (this->lockTarget) {
		if (!this->manager->lock()) this->lockTarget = false;
		this->updateSurfaces(old);
	} else {
		this->setLocked(false);
	}
}

void WlSessionLock::updateSurfaces(WlSessionLock* old) {
	if (this->manager->isLocked()) {
		auto screens = QGuiApplication::screens();

		auto map = this->surfaces.toStdMap();
		for (auto& [screen, surface]: map) {
			if (!screens.contains(screen)) {
				this->surfaces.remove(screen);
				surface->deleteLater();
			}
		}

		if (this->mSurfaceComponent == nullptr) {
			qWarning() << "WlSessionLock.surface is null. Aborting lock.";
			this->unlock();
			return;
		}

		for (auto* screen: screens) {
			if (!this->surfaces.contains(screen)) {
				auto* instanceObj =
				    this->mSurfaceComponent->create(QQmlEngine::contextForObject(this->mSurfaceComponent));
				auto* instance = qobject_cast<WlSessionLockSurface*>(instanceObj);

				if (instance == nullptr) {
					qWarning(
					) << "WlSessionLock.surface does not create a WlSessionLockSurface. Aborting lock.";
					if (instanceObj != nullptr) instanceObj->deleteLater();
					this->unlock();
					return;
				}

				instance->setParent(this);
				instance->setScreen(screen);

				auto* oldInstance = old == nullptr ? nullptr : old->surfaces.value(screen, nullptr);
				instance->onReload(oldInstance);

				this->surfaces[screen] = instance;
			}

			for (auto* surface: this->surfaces.values()) {
				surface->show();
			}
		}
	}
}

void WlSessionLock::unlock() {
	if (this->isLocked()) {
		this->lockTarget = false;
		this->manager->unlock();

		for (auto* surface: this->surfaces) {
			surface->deleteLater();
		}

		this->surfaces.clear();

		emit this->lockStateChanged();
	}
}

void WlSessionLock::onScreensChanged() { this->updateSurfaces(); }

bool WlSessionLock::isLocked() const {
	return this->manager == nullptr ? this->lockTarget : this->manager->isLocked();
}

bool WlSessionLock::isSecure() const {
	return this->manager != nullptr && SessionLockManager::isSecure();
}

void WlSessionLock::setLocked(bool locked) {
	if (this->isLocked() == locked) return;
	this->lockTarget = locked;

	if (this->manager == nullptr) {
		emit this->lockStateChanged();
		return;
	}

	if (locked) {
		if (!this->manager->lock()) this->lockTarget = false;
		this->updateSurfaces();
		if (this->lockTarget) emit this->lockStateChanged();
	} else {
		this->unlock(); // emits lockStateChanged
	}
}

QQmlComponent* WlSessionLock::surfaceComponent() const { return this->mSurfaceComponent; }

void WlSessionLock::setSurfaceComponent(QQmlComponent* surfaceComponent) {
	if (this->mSurfaceComponent != nullptr) this->mSurfaceComponent->deleteLater();
	if (surfaceComponent != nullptr) surfaceComponent->setParent(this);

	this->mSurfaceComponent = surfaceComponent;
	emit this->surfaceComponentChanged();
}

WlSessionLockSurface::WlSessionLockSurface(QObject* parent)
    : Reloadable(parent)
    , mContentItem(new QQuickItem())
    , ext(new LockWindowExtension(this)) {
	QQmlEngine::setObjectOwnership(this->mContentItem, QQmlEngine::CppOwnership);
	this->mContentItem->setParent(this);

	// clang-format off
	QObject::connect(this, &WlSessionLockSurface::widthChanged, this, &WlSessionLockSurface::onWidthChanged);
	QObject::connect(this, &WlSessionLockSurface::heightChanged, this, &WlSessionLockSurface::onHeightChanged);
	// clang-format on
}

WlSessionLockSurface::~WlSessionLockSurface() {
	if (this->window != nullptr) {
		this->window->deleteLater();
	}
}

void WlSessionLockSurface::onReload(QObject* oldInstance) {
	if (auto* old = qobject_cast<WlSessionLockSurface*>(oldInstance)) {
		this->window = old->disownWindow();
	}

	if (this->window == nullptr) {
		this->window = new QQuickWindow();
	}

	this->mContentItem->setParentItem(this->window->contentItem());

	this->mContentItem->setWidth(this->width());
	this->mContentItem->setHeight(this->height());

	if (this->mScreen != nullptr) this->window->setScreen(this->mScreen);
	this->window->setColor(this->mColor);

	// clang-format off
	QObject::connect(this->window, &QWindow::visibilityChanged, this, &WlSessionLockSurface::visibleChanged);
	QObject::connect(this->window, &QWindow::widthChanged, this, &WlSessionLockSurface::widthChanged);
	QObject::connect(this->window, &QWindow::heightChanged, this, &WlSessionLockSurface::heightChanged);
	QObject::connect(this->window, &QWindow::screenChanged, this, &WlSessionLockSurface::screenChanged);
	QObject::connect(this->window, &QQuickWindow::colorChanged, this, &WlSessionLockSurface::colorChanged);
	// clang-format on

	if (auto* parent = qobject_cast<WlSessionLock*>(this->parent())) {
		if (!this->ext->attach(this->window, parent->manager)) {
			qWarning(
			) << "Failed to attach LockWindowExtension to window. Surface will not behave correctly.";
		}
	} else {
		qWarning(
		) << "WlSessionLockSurface parent is not a WlSessionLock. Surface will not behave correctly.";
	}
}

QQuickWindow* WlSessionLockSurface::disownWindow() {
	QObject::disconnect(this->window, nullptr, this, nullptr);
	this->mContentItem->setParentItem(nullptr);

	auto* window = this->window;
	this->window = nullptr;
	return window;
}

void WlSessionLockSurface::show() { this->ext->setVisible(); }

QQuickItem* WlSessionLockSurface::contentItem() const { return this->mContentItem; }

bool WlSessionLockSurface::isVisible() const { return this->window->isVisible(); }

qint32 WlSessionLockSurface::width() const {
	if (this->window == nullptr) return 0;
	else return this->window->width();
}

qint32 WlSessionLockSurface::height() const {
	if (this->window == nullptr) return 0;
	else return this->window->height();
}

QuickshellScreenInfo* WlSessionLockSurface::screen() const {
	QScreen* qscreen = nullptr;

	if (this->window == nullptr) {
		if (this->mScreen != nullptr) qscreen = this->mScreen;
	} else {
		qscreen = this->window->screen();
	}

	return new QuickshellScreenInfo(
	    const_cast<WlSessionLockSurface*>(this), // NOLINT
	    qscreen
	);
}

void WlSessionLockSurface::setScreen(QScreen* qscreen) {
	if (this->mScreen != nullptr) {
		QObject::disconnect(this->mScreen, nullptr, this, nullptr);
	}

	if (qscreen != nullptr) {
		QObject::connect(qscreen, &QObject::destroyed, this, &WlSessionLockSurface::onScreenDestroyed);
	}

	if (this->window == nullptr) {
		this->mScreen = qscreen;
		emit this->screenChanged();
	} else this->window->setScreen(qscreen);
}

void WlSessionLockSurface::onScreenDestroyed() { this->mScreen = nullptr; }

QColor WlSessionLockSurface::color() const {
	if (this->window == nullptr) return this->mColor;
	else return this->window->color();
}

void WlSessionLockSurface::setColor(QColor color) {
	if (this->window == nullptr) {
		this->mColor = color;
		emit this->colorChanged();
	} else this->window->setColor(color);
}

QQmlListProperty<QObject> WlSessionLockSurface::data() {
	return this->mContentItem->property("data").value<QQmlListProperty<QObject>>();
}

void WlSessionLockSurface::onWidthChanged() { this->mContentItem->setWidth(this->width()); }
void WlSessionLockSurface::onHeightChanged() { this->mContentItem->setHeight(this->height()); }
