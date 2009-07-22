<?php

require_once drupal_get_path('module', 'versioncontrol') . '/includes/VersioncontrolBackend.php';

class VersioncontrolGitBackend extends VersioncontrolBackend {

  public function __construct() {
    $this->name = 'Git';
    $this->description = t('Git is a fast, scalable, distributed revision control system with an unusually rich command set that provides both high-level operations and full access to internals.');
    $this->capabilities = array(
        // Use the commit hash for to identify the commit instead of an individual
        // revision for each file.
        VERSIONCONTROL_CAPABILITY_ATOMIC_COMMITS
    );
    $this->flags = array(
        // versioncontrol_insert_repository() will automatically insert
        // array elements from $repository['git_specific'] into
        // {versioncontrol_git_repositories} and versioncontrol_get_repositories()
        // will automatically fetch it from there.
        VERSIONCONTROL_FLAG_AUTOADD_REPOSITORIES,
    );
    $this->classes = array(
      'repo' => 'VersioncontrolGitRepository',
    );
  }

}

class VersioncontrolGitRepository extends VersioncontrolRepository {
}
